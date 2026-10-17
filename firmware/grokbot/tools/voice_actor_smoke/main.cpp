#include "fixture.hpp"
int main(int argc, char **argv) {
    int result = 0;
    try {
        fixture::require(argc >= 2, "scenario required");
        if (argc > 2) host::set_nvs_file(argv[2]);
        const std::string scenario = argv[1];
        if (scenario == "ui" || scenario == "ui-save-failure") fixture::run_ui_scenario(scenario == "ui-save-failure");
        else fixture::run_actor_scenario(scenario);
        std::cout << "PASS " << scenario << ": real actor; reads=" << fixture::reads
                  << " inboxes=" << fixture::inboxes << " commits=" << host::commits
                  << " failed_commits=" << host::failed_commits << '\n';
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; result = 1;
    }
    try { host::shutdown(); }
    catch (const std::exception &error) { std::cerr << "TASK_ERROR: " << error.what() << '\n'; result = 1; }
    std::cout << "all_host_tasks_joined=true\n";
    return result;
}
