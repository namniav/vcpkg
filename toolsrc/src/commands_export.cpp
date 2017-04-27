#include "pch.h"
#include "vcpkg_Commands.h"
#include "vcpkglib.h"
#include "vcpkg_System.h"
#include "vcpkg_Dependencies.h"
#include "vcpkg_Input.h"
#include "vcpkg_Util.h"
#include "Paragraphs.h"
#include <regex>

namespace vcpkg::Commands::Export
{
    using Install::InstallDir;
    using Dependencies::ExportPlanAction;
    using Dependencies::RequestType;
    using Dependencies::ExportPlanType;

    static std::string create_nuspec_file_contents(const std::string& raw_exported_dir,
                                                   const std::string& targets_redirect_path,
                                                   const std::string& nuget_id,
                                                   const std::string& nupkg_version)
    {
        static constexpr auto content_template = R"(
<package>
    <metadata>
        <id>@NUGET_ID@</id>
        <version>@VERSION@</version>
        <authors>vcpkg</authors>
        <description>
            Vcpkg NuGet export
        </description>
    </metadata>
    <files>
        <file src="@RAW_EXPORTED_DIR@\installed\**" target="installed" />
        <file src="@RAW_EXPORTED_DIR@\scripts\**" target="scripts" />
        <file src="@RAW_EXPORTED_DIR@\.vcpkg-root" target="" />
        <file src="@TARGETS_REDIRECT_PATH@" target="build\native\@NUGET_ID@.targets" />
    </files>
</package>
)";

        std::string nuspec_file_content = std::regex_replace(content_template, std::regex("@NUGET_ID@"), nuget_id);
        nuspec_file_content = std::regex_replace(nuspec_file_content, std::regex("@VERSION@"), nupkg_version);
        nuspec_file_content = std::regex_replace(nuspec_file_content, std::regex("@RAW_EXPORTED_DIR@"), raw_exported_dir);
        nuspec_file_content = std::regex_replace(nuspec_file_content, std::regex("@TARGETS_REDIRECT_PATH@"), targets_redirect_path);
        return nuspec_file_content;
    }

    static std::string create_targets_redirect(const std::string& target_path) noexcept
    {
        return Strings::format(R"###(
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Condition="Exists('%s')" Project="%s" />
</Project>
)###", target_path, target_path);
    }

    static void print_plan(const std::map<ExportPlanType, std::vector<const ExportPlanAction*>>& group_by_plan_type)
    {
        static constexpr std::array<ExportPlanType, 2> order = { ExportPlanType::ALREADY_BUILT, ExportPlanType::PORT_AVAILABLE_BUT_NOT_BUILT };

        for (const ExportPlanType plan_type : order)
        {
            auto it = group_by_plan_type.find(plan_type);
            if (it == group_by_plan_type.cend())
            {
                continue;
            }

            std::vector<const ExportPlanAction*> cont = it->second;
            std::sort(cont.begin(), cont.end(), &ExportPlanAction::compare_by_name);
            const std::string as_string = Strings::join("\n", cont, [](const ExportPlanAction* p)
                                                        {
                                                            return Dependencies::to_output_string(p->request_type, p->spec.to_string());
                                                        });

            switch (plan_type)
            {
                case ExportPlanType::ALREADY_BUILT:
                    System::println("The following packages are already built and will be exported:\n%s", as_string);
                    continue;
                case ExportPlanType::PORT_AVAILABLE_BUT_NOT_BUILT:
                    System::println("The following packages need to be built:\n%s", as_string);
                    continue;
                default:
                    Checks::unreachable(VCPKG_LINE_INFO);
            }
        }
    }

    static std::string create_export_id()
    {
        const tm date_time = System::get_current_date_time();

        // Format is: YYYYmmdd-HHMMSS
        // 15 characters + 1 null terminating character will be written for a total of 16 chars
        char mbstr[16];
        const size_t bytes_written = std::strftime(mbstr, sizeof(mbstr), "%Y%m%d-%H%M%S", &date_time);
        Checks::check_exit(VCPKG_LINE_INFO, bytes_written == 15, "Expected 15 bytes to be written, but %u were written", bytes_written);
        const std::string date_time_as_string(mbstr);
        return ("vcpkg-export-" + date_time_as_string);
    }

    static fs::path do_nuget_export(const VcpkgPaths& paths, const fs::path& raw_exported_dir, const fs::path& output_dir)
    {
        static const std::string NUPKG_VERSION = "1.0.0";

        Files::Filesystem& fs = paths.get_filesystem();
        const fs::path& nuget_exe = paths.get_nuget_exe();

        // This file will be placed in "build\native" in the nuget package. Therefore, go up two dirs.
        const std::string targets_redirect_content = create_targets_redirect("../../scripts/buildsystems/msbuild/vcpkg.targets");
        const fs::path targets_redirect = paths.buildsystems / "tmp" / "vcpkg.export.nuget.targets";
        fs.write_contents(targets_redirect, targets_redirect_content);

        const std::string nuget_id = raw_exported_dir.filename().string();
        const std::string nuspec_file_content = create_nuspec_file_contents(raw_exported_dir.string(), targets_redirect.string(), nuget_id, NUPKG_VERSION);
        const fs::path nuspec_file_path = paths.buildsystems / "tmp" / "vcpkg.export.nuspec";
        fs.write_contents(nuspec_file_path, nuspec_file_content);

        // -NoDefaultExcludes is needed for ".vcpkg-root"
        const std::wstring cmd_line = Strings::wformat(LR"("%s" pack -OutputDirectory "%s" "%s" -NoDefaultExcludes > nul)",
                                                       nuget_exe.native(), output_dir.native(), nuspec_file_path.native());

        const int exit_code = System::cmd_execute_clean(cmd_line);
        Checks::check_exit(VCPKG_LINE_INFO, exit_code == 0, "Error: NuGet package creation failed");

        const fs::path output_path = paths.buildsystems / "tmp" / (nuget_id + ".nupkg");
        return output_path;
    }

    struct ArchiveFormat final
    {
        enum class BackingEnum
        {
            ZIP = 1,
            _7ZIP,
        };

        constexpr ArchiveFormat() = delete;

        constexpr ArchiveFormat(BackingEnum backing_enum, const wchar_t* extension, const wchar_t* cmake_option)
            : backing_enum(backing_enum)
            , m_extension(extension)
            , m_cmake_option(cmake_option) { }

        constexpr operator BackingEnum() const { return backing_enum; }
        constexpr CWStringView extension() const { return this->m_extension; }
        constexpr CWStringView cmake_option() const { return this->m_cmake_option; }

    private:
        BackingEnum backing_enum;
        const wchar_t* m_extension;
        const wchar_t* m_cmake_option;
    };

    namespace ArchiveFormatC
    {
        constexpr const ArchiveFormat ZIP(ArchiveFormat::BackingEnum::ZIP, L"zip", L"zip");
        constexpr const ArchiveFormat _7ZIP(ArchiveFormat::BackingEnum::_7ZIP, L"7z", L"7zip");
    }

    static fs::path do_archive_export(const VcpkgPaths& paths, const fs::path& raw_exported_dir, const fs::path& output_dir, const ArchiveFormat& format)
    {
        const fs::path& cmake_exe = paths.get_cmake_exe();

        const std::wstring exported_dir_filename = raw_exported_dir.filename().native();
        const std::wstring exported_archive_filename = Strings::wformat(L"%s.%s", exported_dir_filename, format.extension());
        const fs::path exported_archive_path = (output_dir / exported_archive_filename);

        // -NoDefaultExcludes is needed for ".vcpkg-root"
        const std::wstring cmd_line = Strings::wformat(LR"("%s" -E tar "cf" "%s" --format=%s -- "%s")",
                                                       cmake_exe.native(),
                                                       exported_archive_path.native(),
                                                       format.cmake_option(),
                                                       raw_exported_dir.native());

        const int exit_code = System::cmd_execute_clean(cmd_line);
        Checks::check_exit(VCPKG_LINE_INFO, exit_code == 0, "Error: %s creation failed", exported_archive_path.generic_string());
        return exported_archive_path;
    }

    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths, const Triplet& default_triplet)
    {
        static const std::string OPTION_DRY_RUN = "--dry-run";
        static const std::string OPTION_RAW = "--raw";
        static const std::string OPTION_NUGET = "--nuget";
        static const std::string OPTION_ZIP = "--zip";
        static const std::string OPTION_7ZIP = "--7zip";

        // input sanitization
        static const std::string example = Commands::Help::create_example_string("export zlib zlib:x64-windows boost --nuget");
        args.check_min_arg_count(1, example);

        const std::vector<PackageSpec> specs = Util::fmap(args.command_arguments, [&](auto&& arg)
                                                          {
                                                              return Input::check_and_get_package_spec(arg, default_triplet, example);
                                                          });
        for (auto&& spec : specs)
            Input::check_triplet(spec.triplet(), paths);

        const std::unordered_set<std::string> options = args.check_and_get_optional_command_arguments(
            {
                OPTION_DRY_RUN, OPTION_RAW, OPTION_NUGET, OPTION_ZIP, OPTION_7ZIP
            });
        const bool dryRun = options.find(OPTION_DRY_RUN) != options.cend();
        const bool raw = options.find(OPTION_RAW) != options.cend();
        const bool nuget = options.find(OPTION_NUGET) != options.cend();
        const bool zip = options.find(OPTION_ZIP) != options.cend();
        const bool _7zip = options.find(OPTION_7ZIP) != options.cend();

        Checks::check_exit(VCPKG_LINE_INFO, raw || nuget || zip || _7zip,
                                          "Must provide at least one of the following options: --raw --nuget --zip --7zip");

        // create the plan
        StatusParagraphs status_db = database_load_check(paths);
        std::vector<ExportPlanAction> export_plan = Dependencies::create_export_plan(paths, specs, status_db);
        Checks::check_exit(VCPKG_LINE_INFO, !export_plan.empty(), "Export plan cannot be empty");

        std::map<ExportPlanType, std::vector<const ExportPlanAction*>> group_by_plan_type;
        Util::group_by(export_plan, &group_by_plan_type, [](const ExportPlanAction& p) { return p.plan_type; });
        print_plan(group_by_plan_type);

        const bool has_non_user_requested_packages = Util::find_if(export_plan, [](const ExportPlanAction& package)-> bool
                                                                   {
                                                                       return package.request_type != RequestType::USER_REQUESTED;
                                                                   }) != export_plan.cend();

        if (has_non_user_requested_packages)
        {
            System::println(System::Color::warning, "Additional packages (*) need to be exported to complete this operation.");
        }

        auto it = group_by_plan_type.find(ExportPlanType::PORT_AVAILABLE_BUT_NOT_BUILT);
        if (it != group_by_plan_type.cend() && !it->second.empty())
        {
            System::println(System::Color::error, "There are packages that have not been built.");

            // No need to show all of them, just the user-requested ones. Dependency resolution will handle the rest.
            std::vector<const ExportPlanAction*> unbuilt = it->second;
            Util::erase_remove_if(unbuilt, [](const ExportPlanAction* a)
                                  {
                                      return a->request_type != RequestType::USER_REQUESTED;
                                  });

            auto s = Strings::join(" ", unbuilt, [](const ExportPlanAction* a) { return a->spec.to_string(); });
            System::println("To build them, run:\n"
                            "    vcpkg install %s", s);
            Checks::exit_fail(VCPKG_LINE_INFO);
        }

        if (dryRun)
        {
            Checks::exit_success(VCPKG_LINE_INFO);
        }

        const std::string export_id = create_export_id();

        Files::Filesystem& fs = paths.get_filesystem();
        const fs::path export_to_path = paths.root;
        const fs::path raw_exported_dir_path = export_to_path / export_id;
        std::error_code ec;
        fs.remove_all(raw_exported_dir_path, ec);
        fs.create_directory(raw_exported_dir_path, ec);

        // execute the plan
        for (const ExportPlanAction& action : export_plan)
        {
            if (action.plan_type != ExportPlanType::ALREADY_BUILT)
            {
                Checks::unreachable(VCPKG_LINE_INFO);
            }

            const std::string display_name = action.spec.to_string();
            System::println("Exporting package %s... ", display_name);

            const BinaryParagraph& binary_paragraph = action.any_paragraph.binary_paragraph.value_or_exit(VCPKG_LINE_INFO);
            const InstallDir dirs = InstallDir::from_destination_root(
                raw_exported_dir_path / "installed",
                action.spec.triplet().to_string(),
                raw_exported_dir_path / "installed" / "vcpkg" / "info" / (binary_paragraph.fullstem() + ".list"));

            Install::install_files_and_write_listfile(paths.get_filesystem(), paths.package_dir(action.spec), dirs);
            System::println(System::Color::success, "Exporting package %s... done", display_name);
        }

        // Copy files needed for integration
        const std::vector<fs::path> integration_files_relative_to_root =
        {
            { ".vcpkg-root" },
            { fs::path{ "scripts" } / "buildsystems" / "msbuild" / "applocal.ps1" },
            { fs::path{ "scripts" } / "buildsystems" / "msbuild" / "vcpkg.targets" },
            { fs::path{ "scripts" } / "buildsystems" / "vcpkg.cmake" },
            { fs::path{ "scripts" } / "cmake" / "vcpkg_get_windows_sdk.cmake" },
            { fs::path{ "scripts" } / "getWindowsSDK.ps1" },
            { fs::path{ "scripts" } / "getProgramFilesPlatformBitness.ps1" },
            { fs::path{ "scripts" } / "getProgramFiles32bit.ps1" },
        };

        for (const fs::path& file : integration_files_relative_to_root)
        {
            const fs::path source = paths.root / file;
            const fs::path destination = raw_exported_dir_path / file;
            fs.create_directories(destination.parent_path(), ec);
            Checks::check_exit(VCPKG_LINE_INFO, !ec);
            fs.copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
            Checks::check_exit(VCPKG_LINE_INFO, !ec);
        }

        if (raw)
        {
            System::println(System::Color::success, R"(Files exported at: "%s")", raw_exported_dir_path.generic_string());
        }

        if (nuget)
        {
            System::println("Creating nuget package... ");
            const fs::path output_path = do_nuget_export(paths, raw_exported_dir_path, export_to_path);
            System::println(System::Color::success, "Creating nuget package... done");
            System::println(System::Color::success, "Nuget package exported at: %s", output_path.generic_string());
        }

        if (zip)
        {
            System::println("Creating zip archive... ");
            const fs::path output_path = do_archive_export(paths, raw_exported_dir_path, export_to_path, ArchiveFormatC::ZIP);
            System::println(System::Color::success, "Creating zip archive... done");
            System::println(System::Color::success, "Zip archive exported at: %s", output_path.generic_string());
        }

        if (_7zip)
        {
            System::println("Creating 7zip archive... ");
            const fs::path output_path = do_archive_export(paths, raw_exported_dir_path, export_to_path, ArchiveFormatC::_7ZIP);
            System::println(System::Color::success, "Creating 7zip archive... done");
            System::println(System::Color::success, "7zip archive exported at: %s", output_path.generic_string());
        }

        if (!raw)
        {
            fs.remove_all(raw_exported_dir_path, ec);
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }
}
