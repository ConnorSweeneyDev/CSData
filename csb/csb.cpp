#include "csb.hpp"

#include <format>
#include <string>

void csb::configure()
{
  csb::target_configuration = RELEASE;
  csb::include_files = {"csd.hpp"};
}

int csb::clean()
{
  csb::clean_build();
  return csb::build();
}

int csb::build()
{
  csb::generate_compile_commands(true);
  csb::generate_clangd({{"Diagnostics", {{"UnusedIncludes", "Strict"}, {"MissingIncludes", "Strict"}}}});
  csb::generate_clang_format({{"BasedOnStyle", "LLVM"},
                              {"ColumnLimit", "120"},
                              {"IndentWidth", "2"},
                              {"ConstructorInitializerIndentWidth", "2"},
                              {"ContinuationIndentWidth", "2"},
                              {"Language", "Cpp"},
                              {"BreakBeforeBraces", "Allman"},
                              {"AllowShortBlocksOnASingleLine", "true"},
                              {"AllowShortIfStatementsOnASingleLine", "true"},
                              {"AllowShortCaseLabelsOnASingleLine", "true"},
                              {"AllowShortLoopsOnASingleLine", "true"},
                              {"AllowShortFunctionsOnASingleLine", "true"},
                              {"AllowShortLambdasOnASingleLine", "true"},
                              {"AllowShortEnumsOnASingleLine", "true"},
                              {"AllowShortNamespacesOnASingleLine", "true"},
                              {"BreakTemplateDeclarations", "No"},
                              {"IndentPPDirectives", "BeforeHash"},
                              {"IndentCaseLabels", "true"},
                              {"NamespaceIndentation", "All"},
                              {"FixNamespaceComments", "false"}});
  if (!csb::is_subproject) csb::format("22.1.8");

  auto build_include_path = csb::path("build/include/csd");
  if (!csb::exists(build_include_path)) csb::mkdir(build_include_path);
  csb::multi_task_run(std::format("{} () []", csb::host_platform == WINDOWS ? "copy /Y" : "cp"), csb::include_files,
                      {build_include_path / "(filename)"});
  return csb::run();
}

int csb::run() { return csb::success; }

CSB_MAIN()
