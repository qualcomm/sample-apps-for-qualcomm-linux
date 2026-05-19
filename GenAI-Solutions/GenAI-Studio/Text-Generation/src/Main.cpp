// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include <filesystem>
#include <iostream>
#include <string>

#include "ChatApp.hpp"

namespace
{

constexpr const std::string_view c_option_bundle_dir   = "--bundle-dir";
constexpr const std::string_view c_option_initial_model = "--initial-model";
constexpr const std::string_view c_option_help         = "--help";
constexpr const std::string_view c_option_help_short   = "-h";

void PrintHelp()
{
    std::cout << "\n:::::::: Chat options ::::::::\n\n";
    std::cout << c_option_bundle_dir
              << " <Local directory path>: [Required] Directory containing model bundles.\n"
              << "    Each bundle must be a subfolder containing genie_config.json.\n";
    std::cout << c_option_initial_model
              << " <Bundle name>: [Optional] Initial bundle folder name to load.\n"
              << "    If omitted, the server loads the alphabetically-first discovered bundle.\n";
}

} // namespace

int main(int argc, char* argv[])
{
    std::string bundle_dir;
    std::string initial_model;
    bool invalid_arguments = false;

    auto check_dir_path = [&invalid_arguments](const std::string_view& arg_name, const std::string& value)
    {
        if (value.empty() || !std::filesystem::exists(value) || !std::filesystem::is_directory(value))
        {
            std::cout << "\nInvalid argument for " << arg_name
                      << ": It must be a valid and accessible local directory path. Provided: " << value << std::endl;
            invalid_arguments = true;
        }
    };

    for (int i = 1; i < argc; ++i)
    {
        if (c_option_bundle_dir == argv[i])
        {
            if (i + 1 < argc)
            {
                bundle_dir = argv[++i];
                check_dir_path(c_option_bundle_dir, bundle_dir);
            }
            else
            {
                std::cout << "\nMissing value for " << c_option_bundle_dir << " option.\n";
                invalid_arguments = true;
            }
        }
        else if (c_option_initial_model == argv[i])
        {
            if (i + 1 < argc)
            {
                initial_model = argv[++i];
            }
            else
            {
                std::cout << "\nMissing value for " << c_option_initial_model << " option.\n";
                invalid_arguments = true;
            }
        }
        else if (c_option_help == argv[i] || c_option_help_short == argv[i])
        {
            PrintHelp();
            return 0;
        }
        else
        {
            std::cout << "Unsupported option " << argv[i] << " provided.\n";
            invalid_arguments = true;
        }
    }

    if (invalid_arguments || bundle_dir.empty())
    {
        PrintHelp();
        return 1;
    }

    try
    {
        App::ChatApp app(bundle_dir, initial_model);
        app.Run(8088);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown error.\n";
        return 1;
    }
    return 0;
}