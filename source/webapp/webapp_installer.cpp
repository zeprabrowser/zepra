/**
 * @file webapp_installer.cpp
 * @brief Web App Installer Implementation
 * 
 * Installs Progressive Web Apps as native NeolyxOS applications.
 * 
 * Copyright (c) 2025 KetiveeAI
 */

#include "webapp_installer.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "platform/platform_compat.h"
#ifndef _WIN32
#  include <sys/stat.h>
#  include <dirent.h>
#endif

namespace Zepra::WebApp {

WebAppInstaller::WebAppInstaller() {
    std::string home = Zepra::Platform::getHomeDirectory();
    std::string sep(1, Zepra::Platform::pathSeparator());
    if (!home.empty()) {
        install_base_ = home + sep + "Applications" + sep;
    } else {
        install_base_ = "." + sep + "Applications" + sep;
    }
}

WebAppInstaller::~WebAppInstaller() = default;

std::string WebAppInstaller::expandPath(const std::string& path) const {
    return Zepra::Platform::expandHomePath(path);
}

std::string WebAppInstaller::getInstallPath(const std::string& app_name) const {
    // Sanitize app name (remove spaces, special chars)
    std::string safe_name;
    for (char c : app_name) {
        if (isalnum(c) || c == '-' || c == '_') {
            safe_name += c;
        }
    }
    return install_base_ + safe_name + "/";
}

bool WebAppInstaller::isInstalled(const std::string& app_id) const {
#ifndef _WIN32
    // NeolyxOS/Linux: scan Applications directory
    std::string apps_dir = expandPath(install_base_);
    DIR* dir = opendir(apps_dir.c_str());
    if (!dir) return false;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            std::string info_path = apps_dir + entry->d_name + "/Info.nxpt";
            std::ifstream f(info_path);
            if (f.good()) {
                std::stringstream buf;
                buf << f.rdbuf();
                std::string content = buf.str();
                if (content.find("\"NXAppId\": \"" + app_id + "\"") != std::string::npos) {
                    closedir(dir);
                    return true;
                }
            }
        }
    }
    closedir(dir);
    return false;
#else
    (void)app_id;
    return false;  // Web app install is NeolyxOS/Linux feature
#endif
}

bool WebAppInstaller::createInfoNxpt(const std::string& path, 
                                      const WebAppManifest& manifest) {
    std::ofstream f(path + "Info.nxpt");
    if (!f) {
        last_error_ = "Failed to create Info.nxpt";
        return false;
    }
    
    // Generate app ID if not provided
    std::string app_id = manifest.nx_app_id.empty() 
        ? generateAppId(manifest) 
        : manifest.nx_app_id;
    
    f << "{\n";
    f << "    \"NXAppName\": \"" << manifest.name << "\",\n";
    f << "    \"NXAppId\": \"" << app_id << "\",\n";
    f << "    \"NXAppVersion\": \"1.0.0\",\n";
    f << "    \"NXExecutable\": \"launch\",\n";
    f << "    \"NXIcon\": \"Resources/icon.png\",\n";
    f << "    \"NXCategory\": \"Web Apps\",\n";
    f << "    \"NXWebApp\": true,\n";
    f << "    \"NXWebAppURL\": \"" << manifest.start_url << "\",\n";
    f << "    \"NXWebAppEngine\": \"zeprascript\",\n";
    f << "    \"NXCapabilities\": [\n";
    f << "        \"network.client\",\n";
    f << "        \"gpu.render\"";
    
    // Add manifest-specified capabilities
    for (const auto& cap : manifest.nx_capabilities) {
        f << ",\n        \"" << cap << "\"";
    }
    
    f << "\n    ],\n";
    f << "    \"NXMinOS\": \"1.0\",\n";
    f << "    \"NXCopyright\": \"Installed via ZepraBrowser\"\n";
    f << "}\n";
    
    f.close();
    return true;
}

bool WebAppInstaller::createLauncher(const std::string& path, 
                                      const WebAppManifest& manifest) {
    std::string launcher_path = path + "launch";
    std::ofstream f(launcher_path);
    if (!f) {
        last_error_ = "Failed to create launcher";
        return false;
    }
    
    // Create shell script that launches ZepraBrowser in app mode
    f << "#!/bin/bash\n";
    f << "# NeolyxOS Web App Launcher\n";
    f << "# App: " << manifest.name << "\n";
    f << "\n";
    f << "exec /Applications/ZepraBrowser/ZepraBrowser \\\n";
    f << "    --app=\"" << manifest.start_url << "\" \\\n";
    f << "    --app-id=\"" << (manifest.nx_app_id.empty() ? generateAppId(manifest) : manifest.nx_app_id) << "\" \\\n";
    
    if (manifest.display == DisplayMode::Fullscreen) {
        f << "    --fullscreen \\\n";
    }
    
    f << "    \"$@\"\n";
    
    f.close();
    
#ifndef _WIN32
    chmod(launcher_path.c_str(), 0755);
#endif
    
    return true;
}

bool WebAppInstaller::downloadIcon(const std::string& icon_url, 
                                    const std::string& dest_path) {
    // Use curl to download icon
    std::string cmd = "curl -sL \"" + icon_url + "\" -o \"" + dest_path + "\"";
    int result = system(cmd.c_str());
    if (result != 0) {
        last_error_ = "Failed to download icon from " + icon_url;
        return false;
    }
    return true;
}

InstallResult WebAppInstaller::install(const WebAppManifest& manifest,
                                        ProgressCallback callback) {
    if (callback) callback(0, "Starting installation...");
    
    // Generate app ID
    std::string app_id = manifest.nx_app_id.empty() 
        ? generateAppId(manifest) 
        : manifest.nx_app_id;
    
    // Check if already installed
    if (isInstalled(app_id)) {
        last_error_ = "App is already installed";
        return InstallResult::AlreadyInstalled;
    }
    
    if (callback) callback(10, "Creating app directory...");
    
    // Create app directory
    std::string app_path = getInstallPath(manifest.short_name.empty() 
        ? manifest.name 
        : manifest.short_name);
    std::string resources_path = app_path + "Resources/";
    
    // Create directories
    Zepra::Platform::createDirectory(expandPath(install_base_).c_str());
    Zepra::Platform::createDirectory(app_path.c_str());
    Zepra::Platform::createDirectory(resources_path.c_str());
    
    if (callback) callback(30, "Creating Info.nxpt...");
    
    // Create Info.nxpt
    if (!createInfoNxpt(app_path, manifest)) {
        return InstallResult::FileSystemError;
    }
    
    if (callback) callback(50, "Downloading icon...");
    
    // Download icon
    std::string icon_url = getBestIcon(manifest, 256);
    if (!icon_url.empty()) {
        downloadIcon(icon_url, resources_path + "icon.png");
    }
    
    if (callback) callback(70, "Creating launcher...");
    
    // Create launcher script
    if (!createLauncher(app_path, manifest)) {
        return InstallResult::FileSystemError;
    }
    
    if (callback) callback(100, "Installation complete!");
    
    return InstallResult::Success;
}

InstallResult WebAppInstaller::installFromUrl(const std::string& url,
                                               ProgressCallback callback) {
    WebAppManifest manifest;
    
    if (callback) callback(0, "Fetching manifest...");
    
    if (!fetchManifest(url, manifest)) {
        last_error_ = "Failed to fetch or parse manifest";
        return InstallResult::ManifestError;
    }
    
    return install(manifest, callback);
}

bool WebAppInstaller::uninstall(const std::string& app_id) {
#ifndef _WIN32
    std::string apps_dir = expandPath(install_base_);
    DIR* dir = opendir(apps_dir.c_str());
    if (!dir) return false;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            std::string app_path = apps_dir + entry->d_name + "/";
            std::string info_path = app_path + "Info.nxpt";
            
            std::ifstream f(info_path);
            if (f.good()) {
                std::stringstream buf;
                buf << f.rdbuf();
                std::string content = buf.str();
                
                if (content.find("\"NXAppId\": \"" + app_id + "\"") != std::string::npos) {
                    f.close();
                    closedir(dir);
                    std::string cmd = "rm -rf \"" + app_path + "\"";
                    return system(cmd.c_str()) == 0;
                }
            }
        }
    }
    closedir(dir);
    return false;
#else
    (void)app_id;
    return false;
#endif
}

/* Helper functions */

std::string generateAppId(const WebAppManifest& manifest) {
    // Generate from start_url domain + name
    std::string id = "web.";
    
    // Extract domain from start_url
    std::string url = manifest.start_url;
    size_t start = url.find("://");
    if (start != std::string::npos) {
        start += 3;
        size_t end = url.find('/', start);
        std::string domain = url.substr(start, end - start);
        
        // Reverse domain (example.com -> com.example)
        size_t dot = domain.rfind('.');
        if (dot != std::string::npos) {
            id += domain.substr(dot + 1) + ".";
            id += domain.substr(0, dot);
        } else {
            id += domain;
        }
    }
    
    id += ".";
    
    // Add app name (sanitized)
    for (char c : manifest.short_name.empty() ? manifest.name : manifest.short_name) {
        if (isalnum(c)) {
            id += tolower(c);
        }
    }
    
    return id;
}

std::string getBestIcon(const WebAppManifest& manifest, int size) {
    std::string best;
    int best_diff = 99999;
    
    for (const auto& icon : manifest.icons) {
        // Parse size (e.g., "256x256")
        int w = 0;
        sscanf(icon.sizes.c_str(), "%d", &w);
        
        int diff = abs(w - size);
        if (diff < best_diff) {
            best_diff = diff;
            best = icon.src;
        }
    }
    
    return best;
}

DisplayMode parseDisplayMode(const std::string& str) {
    if (str == "standalone") return DisplayMode::Standalone;
    if (str == "fullscreen") return DisplayMode::Fullscreen;
    if (str == "minimal-ui") return DisplayMode::MinimalUI;
    return DisplayMode::Browser;
}

bool quickInstall(const std::string& url) {
    WebAppInstaller installer;
    auto result = installer.installFromUrl(url, nullptr);
    return result == InstallResult::Success;
}

std::vector<std::string> listInstalledWebApps() {
    std::vector<std::string> apps;
#ifndef _WIN32
    std::string home = Zepra::Platform::getHomeDirectory();
    if (home.empty()) return apps;
    std::string apps_dir = home + "/Applications/";
    DIR* dir = opendir(apps_dir.c_str());
    if (!dir) return apps;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            std::string info_path = apps_dir + entry->d_name + "/Info.nxpt";
            std::ifstream f(info_path);
            if (f.good()) {
                std::stringstream buf;
                buf << f.rdbuf();
                std::string content = buf.str();
                if (content.find("\"NXWebApp\": true") != std::string::npos) {
                    size_t pos = content.find("\"NXAppId\": \"");
                    if (pos != std::string::npos) {
                        pos += 12;
                        size_t end = content.find('"', pos);
                        if (end != std::string::npos) {
                            apps.push_back(content.substr(pos, end - pos));
                        }
                    }
                }
            }
        }
    }
    closedir(dir);
#endif
    return apps;
}

/* Manifest parser - simple JSON parsing */
bool parseManifest(const std::string& json_content, 
                   const std::string& base_url,
                   WebAppManifest& out) {
    // Simple JSON key extraction
    auto extract = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = json_content.find(search);
        if (pos == std::string::npos) return "";
        
        pos = json_content.find_first_of("\"", pos + search.length());
        if (pos == std::string::npos) return "";
        pos++;
        
        size_t end = json_content.find('"', pos);
        if (end == std::string::npos) return "";
        
        return json_content.substr(pos, end - pos);
    };
    
    out.name = extract("name");
    out.short_name = extract("short_name");
    out.start_url = extract("start_url");
    out.description = extract("description");
    out.background_color = extract("background_color");
    out.theme_color = extract("theme_color");
    out.display = parseDisplayMode(extract("display"));
    
    // Resolve relative URLs
    if (!out.start_url.empty() && out.start_url[0] == '/') {
        out.start_url = base_url + out.start_url;
    }
    
    // Parse icons (simplified)
    // Full implementation would parse the icons array properly
    
    return !out.name.empty();
}

bool fetchManifest(const std::string& manifest_url, WebAppManifest& out) {
    // Fetch using curl
    std::string cmd = "curl -sL \"" + manifest_url + "\"";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    
    std::string result;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    
    // Extract base URL
    size_t pos = manifest_url.rfind('/');
    std::string base_url = (pos != std::string::npos) 
        ? manifest_url.substr(0, pos) 
        : manifest_url;
    
    return parseManifest(result, base_url, out);
}

} // namespace Zepra::WebApp
