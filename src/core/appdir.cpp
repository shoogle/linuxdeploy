// library headers
#include <boost/filesystem.hpp>
#include <Magick++.h>
#include <fnmatch.h>
#include <dirent.h>
#include <fts.h>
#include <subprocess.hpp>

// local headers
#include "linuxdeploy/core/appdir.h"
#include "linuxdeploy/core/elf.h"
#include "linuxdeploy/core/log.h"
#include "linuxdeploy/core/util.h"
#include "excludelist.h"

using namespace linuxdeploy::core;
using namespace linuxdeploy::core::log;

namespace bf = boost::filesystem;

namespace linuxdeploy {
    namespace core {
        namespace appdir {
            class AppDir::PrivateData {
                public:
                    bf::path appDirPath;
                    std::map<bf::path, bf::path> copyOperations;
                    std::vector<bf::path> setElfRPathOperations;

                public:
                    PrivateData() {
                        this->copyOperations = {};
                    };

                public:
                    // actually copy file
                    // mimics cp command behavior
                    bool copyFile(const bf::path& from, bf::path to) {
                        ldLog() << "Copying file" << from << "to" << to << std::endl;

                        try {
                            if (!to.parent_path().empty() && !bf::is_directory(to.parent_path()) && !bf::create_directories(to.parent_path())) {
                                ldLog() << LD_ERROR << "Failed to create parent directory" << to.parent_path() << "for path" << to << std::endl;
                                return false;
                            }

                            if (*(to.string().end() - 1) == '/' || bf::is_directory(to))
                                to /= from.filename();

                            bf::copy_file(from, to, bf::copy_option::overwrite_if_exists);
                        } catch (const bf::filesystem_error& e) {
                            return false;
                        }

                        return true;
                    }

                    // create symlink
                    bool symlinkFile(bf::path target, bf::path symlink, const bool useRelativePath = true) {
                        ldLog() << "Creating symlink for file" << target << "in/as" << symlink << std::endl;

                        /*try {
                            if (!symlink.parent_path().empty() && !bf::is_directory(symlink.parent_path()) && !bf::create_directories(symlink.parent_path())) {
                                ldLog() << LD_ERROR << "Failed to create parent directory" << symlink.parent_path() << "for path" << symlink << std::endl;
                                return false;
                            }

                            if (*(symlink.string().end() - 1) == '/' || bf::is_directory(symlink))
                                symlink /= target.filename();

                            if (bf::exists(symlink) || bf::symbolic_link_exists(symlink))
                                bf::remove(symlink);

                            if (relativeDirectory != "") {
                                // TODO
                            }

                            bf::create_symlink(target, symlink);
                        } catch (const bf::filesystem_error& e) {
                            return false;
                        }*/

                        if (!useRelativePath) {
                            ldLog() << LD_ERROR << "Not implemented" << std::endl;
                            return false;
                        }

                        subprocess::Popen proc({"ln", "-f", "-s", "--relative", target.c_str(), symlink.c_str()},
                            subprocess::output(subprocess::PIPE),
                            subprocess::error(subprocess::PIPE)
                        );

                        auto outputs = proc.communicate();

                        if (proc.retcode() != 0) {
                            ldLog() << LD_ERROR << "ln subprocess failed:" << std::endl
                                    << outputs.first.buf << std::endl << outputs.second.buf << std::endl;
                            return false;
                        }

                        return true;
                    }

                    bool checkDuplicate(const bf::path& path) {
                        // FIXME: use more efficient search (e.g., binary search)
                        // a linear search is not _really_ efficient
                        //return std::binary_search(copyOperations.begin(), copyOperations.end(), path);
                        for (const auto& pair : copyOperations) {
                            if (pair.first == path) {
                                ldLog() << LD_DEBUG << "Duplicate:" << pair.first << std::endl;
                                return true;
                            }
                        }

                        return false;
                    }

                    // execute deferred copy operations registered with the deploy* functions
                    bool executeDeferredOperations() {
                        bool success = true;

                        while (!copyOperations.empty()) {
                            const auto& pair = *(copyOperations.begin());
                            const auto& from = pair.first;
                            const auto& to = pair.second;

                            if (!copyFile(from, to)) {
                                ldLog() << LD_ERROR << "Failed to copy file" << from << "to" << to << std::endl;
                                success = false;
                            }

                            copyOperations.erase(copyOperations.begin());
                        }

                        if (success) {
                            while (!setElfRPathOperations.empty()) {
                                static const auto rpath = "$ORIGIN/../lib";
                                const auto& elfFilePath = *(setElfRPathOperations.begin());

                                ldLog() << "Setting rpath in ELF file" << elfFilePath << "to" << rpath << std::endl;
                                if (!elf::ElfFile(elfFilePath).setRPath(rpath)) {
                                    ldLog() << LD_ERROR << "Failed to set rpath in ELF file:" << elfFilePath << std::endl;
                                    success = false;
                                }

                                setElfRPathOperations.erase(setElfRPathOperations.begin());
                            }
                        }

                        return success;
                    }

                    // register copy operation that will be executed later
                    // by compiling a list of files to copy instead of just copying everything, one can ensure that
                    // the files are touched once only
                    void deployFile(const bf::path& from, bf::path to) {
                        ldLog() << LD_DEBUG << "Deploying file" << from << "to" << to << std::endl;

                        // not sure whether this is 100% bullet proof, but it simulates the cp command behavior
                        if (to.string().back() == '/' || bf::is_directory(to)) {
                            to /= from.filename();
                        }

                        copyOperations[from] = to;
                    }

                    bool deployElfDependencies(const bf::path& path) {
                        ldLog() << "Deploying dependencies for ELF file" << path << std::endl;
                        
                        for (const auto& dependencyPath : elf::ElfFile(path).traceDynamicDependencies()) {
                            if (!deployLibrary(dependencyPath))
                                return false;
                        }
                        
                        return true;
                    }

                    bool deployLibrary(const bf::path& path) {
                        if (checkDuplicate(path)) {
                            ldLog() << LD_DEBUG << "Skipping duplicate deployment of shared library" << path << std::endl;
                            return true;
                        }

                        static auto isInExcludelist = [](const bf::path& fileName) {
                            for (const auto& excludePattern : generatedExcludelist) {
                                // simple string match is faster than using fnmatch
                                if (excludePattern == fileName)
                                    return true;

                                auto fnmatchResult = fnmatch(excludePattern.c_str(), fileName.string().c_str(), FNM_PATHNAME);
                                switch (fnmatchResult) {
                                    case 0:
                                        return true;
                                    case FNM_NOMATCH:
                                        break;
                                    default:
                                        ldLog() << LD_ERROR << "fnmatch() reported error:" << fnmatchResult << std::endl;
                                        return false;
                                }
                            }

                            return false;
                        };

                        if (isInExcludelist(path.filename())) {
                            ldLog() << "Skipping deployment of blacklisted library" << path << std::endl;
                            return true;
                        } else {
                            ldLog() << "Deploying shared library" << path << std::endl;
                        }

                        deployFile(path, appDirPath / "usr/lib/");

                        setElfRPathOperations.push_back(appDirPath / "usr/lib" / path.filename());

                        if (!deployElfDependencies(path))
                            return false;

                        return true;
                    }

                    bool deployExecutable(const bf::path& path) {
                        if (checkDuplicate(path)) {
                            ldLog() << LD_DEBUG << "Skipping duplicate deployment of executable" << path << std::endl;
                            return true;
                        }

                        ldLog() << "Deploying executable" << path << std::endl;

                        // FIXME: make executables executable

                        deployFile(path, appDirPath / "usr/bin/");

                        setElfRPathOperations.push_back(appDirPath / "usr/bin" / path.filename());

                        if (!deployElfDependencies(path))
                            return false;

                        return true;
                    }

                    bool deployDesktopFile(const desktopfile::DesktopFile& desktopFile) {
                        if (checkDuplicate(desktopFile.path())) {
                            ldLog() << LD_DEBUG << "Skipping duplicate deployment of desktop file" << desktopFile.path() << std::endl;
                            return true;
                        }

                        if (desktopFile.validate()) {
                            ldLog() << LD_ERROR << "Failed to verify desktop file:" << desktopFile.path() << std::endl;
                        }

                        ldLog() << "Deploying desktop file" << desktopFile.path() << std::endl;

                        deployFile(desktopFile.path(), appDirPath / "usr/share/applications/");

                        return true;
                    }

                    bool deployIcon(const bf::path& path) {
                        if (checkDuplicate(path)) {
                            ldLog() << LD_DEBUG << "Skipping duplicate deployment of icon" << path << std::endl;
                            return true;
                        }

                        ldLog() << "Deploying icon" << path << std::endl;

                        Magick::Image image;

                        try {
                            image.read(path.string());
                        } catch (const Magick::Exception& error) {
                            return false;
                        }

                        auto xRes = image.columns();
                        auto yRes = image.rows();

                        if (xRes != yRes) {
                            ldLog() << LD_WARNING << "x and y resolution of icon are not equal:" << path;
                        }

                        auto resolution = std::to_string(xRes) + "x" + std::to_string(yRes);

                        auto format = image.format();

                        // if file is a vector image, use "scalable" directory
                        if (util::strLower(bf::extension(path)) == "svg") {
                            resolution = "scalable";
                        } else {
                            // otherwise, test resolution against "known good" values, and reject invalid ones
                            const auto knownResolutions = {8, 16, 20, 22, 24, 32, 48, 64, 72, 96, 128, 192, 256, 512};

                            // assume invalid
                            bool invalidXRes = true, invalidYRes = true;

                            for (const auto res : knownResolutions) {
                                if (xRes == res)
                                    invalidXRes = false;
                                if (yRes == res)
                                    invalidYRes = false;
                            }

                            if (invalidXRes) {
                                ldLog() << LD_ERROR << "Icon" << path << "has invalid x resolution:" << xRes;
                                return false;
                            }

                            if (invalidYRes) {
                                ldLog() << LD_ERROR << "Icon" << path << "has invalid x resolution:" << xRes;
                                return false;
                            }
                        }

                        deployFile(path, appDirPath / "usr/share/icons/hicolor" / resolution / "apps/");

                        return true;
                    }
            };

            AppDir::AppDir(const bf::path& path) {
                d = new PrivateData();

                d->appDirPath = path;
            }

            AppDir::~AppDir() {
                delete d;
            }

            AppDir::AppDir(const std::string& path) : AppDir(bf::path(path)) {}

            bool AppDir::createBasicStructure() {
                std::vector<std::string> dirPaths = {
                    "usr/bin/",
                    "usr/lib/",
                    "usr/share/applications/",
                    "usr/share/icons/hicolor/",
                };

                for (const std::string& resolution : {"16x16", "32x32", "64x64", "128x128", "256x256", "scalable"}) {
                    auto iconPath = "usr/share/icons/hicolor/" + resolution + "/apps/";
                    dirPaths.push_back(iconPath);
                }

                for (const auto& dirPath : dirPaths) {
                    auto fullDirPath = d->appDirPath / dirPath;

                    ldLog() << "Creating directory" << fullDirPath << std::endl;

                    // skip directory if it exists
                    if (bf::is_directory(fullDirPath))
                        continue;

                    try {
                        bf::create_directories(fullDirPath);
                    } catch (const bf::filesystem_error&) {
                        ldLog() << LD_ERROR << "Failed to create directory" << fullDirPath;
                        return false;
                    }
                }

                return true;
            }

            bool AppDir::deployLibrary(const bf::path& path) {
                return d->deployLibrary(path);
            }

            bool AppDir::deployExecutable(const bf::path& path) {
                return d->deployExecutable(path);
            }

            bool AppDir::deployDesktopFile(const desktopfile::DesktopFile& desktopFile) {
                return d->deployDesktopFile(desktopFile);
            }
 
            bool AppDir::deployIcon(const bf::path& path) {
                return d->deployIcon(path);
            }

            bool AppDir::executeDeferredOperations() {
                return d->executeDeferredOperations();
            }

            boost::filesystem::path AppDir::path() {
                return d->appDirPath;
            }

            static std::vector<bf::path> listFilesInDirectory(const bf::path& path, const bool recursive = true) {
                std::vector<bf::path> foundPaths;

                std::vector<char> pathBuf(path.string().size() + 1, '\0');
                strcpy(pathBuf.data(), path.string().c_str());

                std::vector<char*> searchPaths = {pathBuf.data(), nullptr};

                if (recursive) {
                    // reset errno
                    errno = 0;

                    auto* fts = fts_open(searchPaths.data(), FTS_NOCHDIR | FTS_NOSTAT, nullptr);

                    int error = errno;
                    if (fts == nullptr || error != 0) {
                        ldLog() << LD_ERROR << "fts() failed:" << strerror(error) << std::endl;
                        return {};
                    }

                    FTSENT* ent;
                    while ((ent = fts_read(fts)) != nullptr) {
                        // FIXME: use ent's fts_info member instead of boost::filesystem
                        if (bf::is_regular_file(ent->fts_path)) {
                            foundPaths.push_back(ent->fts_path);
                        }
                    };
                    error = errno;
                    if (error != 0) {
                        ldLog() << LD_ERROR << "fts_read() failed:" << strerror(error) << std::endl;
                        return {};
                    }
                } else {
                    DIR* dir;
                    if ((dir = opendir(path.string().c_str())) == NULL) {
                        auto error = errno;
                        ldLog() << LD_ERROR << "opendir() failed:" << strerror(error);
                    }

                    struct dirent* ent;
                    while ((ent = readdir(dir)) != NULL)  {
                        auto fullPath = path / bf::path(ent->d_name);
                        if (bf::is_regular_file(fullPath)) {
                            foundPaths.push_back(fullPath);
                        }
                    }
                }

                return foundPaths;
            }

            std::vector<bf::path> AppDir::deployedIconPaths() {
                return listFilesInDirectory(path() / "/usr/share/icons/");
            }

            std::vector<bf::path> AppDir::deployedExecutablePaths() {
                auto paths = listFilesInDirectory(path() / "usr/bin/", false);

                paths.erase(std::remove_if(paths.begin(), paths.end(), [](const bf::path& path) {
                    return !bf::is_regular_file(path);
                }), paths.end());

                return paths;
            }

            std::vector<desktopfile::DesktopFile> AppDir::deployedDesktopFiles() {
                std::vector<desktopfile::DesktopFile> desktopFiles;

                auto paths = listFilesInDirectory(path() / "usr/share/applications/", false);
                paths.erase(std::remove_if(paths.begin(), paths.end(), [](const bf::path& path) {
                    return path.extension() != ".desktop";
                }), paths.end());

                for (const auto& path : paths) {
                    desktopFiles.push_back(desktopfile::DesktopFile(path));
                }

                return desktopFiles;
            }

            bool AppDir::createLinksInAppDirRoot(const desktopfile::DesktopFile& desktopFile) {
                ldLog() << "Deploying desktop file to AppDir root:" << desktopFile.path() << std::endl;

                // copy desktop file to root directory
                if (!d->symlinkFile(desktopFile.path(), path())) {
                    ldLog() << LD_ERROR << "Failed to create link to desktop file in AppDir root:" << desktopFile.path() << std::endl;
                    return false;
                }

                // look for suitable icon
                std::string iconName;

                if (!desktopFile.getEntry("Desktop Entry", "Icon", iconName)) {
                    ldLog() << LD_ERROR << "Icon entry missing in desktop file:" << desktopFile.path() << std::endl;
                    return false;
                }

                const auto foundIconPaths = deployedIconPaths();

                if (foundIconPaths.empty()) {
                    ldLog() << LD_ERROR << "Could not find suitable executable for Exec entry:" << iconName << std::endl;
                    return false;
                }

                for (const auto& iconPath : foundIconPaths) {
                    ldLog() << LD_DEBUG << "Icon found:" << iconPath << std::endl;

                    if (iconPath.stem() == iconName) {
                        ldLog() << "Deploying icon to AppDir root:" << iconPath << std::endl;

                        if (!d->symlinkFile(iconPath, path())) {
                            ldLog() << LD_ERROR << "Failed to create symlink for icon in AppDir root:" << iconPath << std::endl;
                            return false;
                        }
                    }
                }

                // look for suitable binary to create AppRun symlink
                std::string executableName;

                if (!desktopFile.getEntry("Desktop Entry", "Exec", executableName)) {
                    ldLog() << LD_ERROR << "Exec entry missing in desktop file:" << desktopFile.path() << std::endl;
                    return false;
                }

                const auto foundExecutablePaths = deployedExecutablePaths();

                if (foundExecutablePaths.empty()) {
                    ldLog() << LD_ERROR << "Could not find suitable executable for Exec entry:" << iconName << std::endl;
                    return false;
                }

                for (const auto& executablePath : foundExecutablePaths) {
                    ldLog() << LD_DEBUG << "Executable found:" << executablePath << std::endl;

                    if (executablePath.stem() == iconName) {
                        ldLog() << "Deploying AppRun symlink for executable in AppDir root:" << executablePath << std::endl;

                        if (!d->symlinkFile(executablePath, path() / "AppRun")) {
                            ldLog() << LD_ERROR << "Failed to create AppRun symlink for executable in AppDir root:" << executablePath << std::endl;
                            return false;
                        }
                    }
                }

                return true;
            }
        }
    }
}
