// Example: emit log records from 8 concurrent threads into the binary file
// ("file.bin") sink.
//
// P8 is initialized once with a JSON config that selects the file.bin sink and
// points its "FileBin"."OutDir" at an output directory (argv[1], or ./p8_logs
// by default). Eight worker threads then log concurrently through the shared,
// lock-free client API; p8_release() flushes everything to disk on shutdown.
//
// The sink writes a per-run subdirectory containing <name>.p8svc (service
// descriptors) and <name>.p8dat (the hello header plus all log data).

#include "p8_client_api.h"
#include "p8_profiler.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(P8_ENABLE_TRACY)
    #if defined(_WIN32)
        #include <conio.h>
    #else
        #include <termios.h>
        #include <unistd.h>
    #endif
void wait_for_any_key();
#endif

namespace
{
static constexpr uint32_t lu_thread_count    = 4;
static constexpr uint32_t lu_logs_per_thread = 1'000'000;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Worker body: name the thread (for nice formatting) and emit iu_logs records.
// cp8_thread registers/unregisters the thread name via RAII around the emission
// loop. Named log modules are not implemented yet, so records are sent with a
// null module (matching the current engine behavior).
void run_worker(uint32_t iu_thread_index, uint32_t iu_logs)
{
    char la_name[32];
    std::snprintf(la_name, sizeof(la_name), "worker_%u", iu_thread_index);
    cp8_thread lo_thread_guard(la_name);

    // No-ops unless the build was configured with a *-tracy preset.
    P8_PROF_THREAD_NAME(la_name);
    P8_PROF_ZONE_NAMED("worker_log_loop");

    for(uint32_t lu_i = 0; lu_i < iu_logs; ++lu_i)
    {
        p8_log_sent(e_p8_info0,
                    nullptr, // module handles are not implemented yet
                    0,       // no trace correlation
                    static_cast<uint32_t>(__LINE__),
                    __FILE__,
                    __FUNCTION__,
                    0, // no attributes
                    nullptr,
                    "thread %u message %u of %u",
                    iu_thread_index,
                    lu_i + 1,
                    iu_logs);
    }
}
} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv)
{
    const char *lp_out_dir       = (argc > 1) ? argv[1] : "./p8_logs";

    const std::string ls_config  = std::string(R"(
    {
        "max_memory_size": "8MB",
        "initial_memory_size": "8MB",
        "sink": "file.bin",
        "FileBin": 
        { 
             "OutDir": ")") + lp_out_dir
                                   + R"(" 
        }
    })";

    struct s_p8_config lo_config = {};
    lo_config.mp_json_config     = ls_config.c_str();

    if(!p8_initialize(&lo_config))
    {
        std::fprintf(stderr, "failed to initialize p8\n");
        return EXIT_FAILURE;
    }

    std::printf("emitting %u logs from %u threads into \"%s\" ...\n", lu_logs_per_thread, lu_thread_count, lp_out_dir);

    {
        // No-op unless the build was configured with a *-tracy preset.
        P8_PROF_ZONE_NAMED("emit_all_threads");

        std::vector<std::thread> lo_threads;
        lo_threads.reserve(lu_thread_count);
        for(uint32_t lu_t = 0; lu_t < lu_thread_count; ++lu_t)
        {
            lo_threads.emplace_back(run_worker, lu_t, lu_logs_per_thread);
        }

        for(auto &lo_t : lo_threads)
        {
            lo_t.join();
        }
    }

    // p8_release() drains buffered records to the sink and tears the core down.
    P8_PROF_ZONE_NAMED("p8_release");
    p8_release();

    std::printf("done: %u threads x %u logs delivered to the file.bin sink\n", lu_thread_count, lu_logs_per_thread);

#if defined(P8_ENABLE_TRACY)
    // Keep the process (and the Tracy frame) alive until the user presses any
    // key, so a Tracy profiler client has time to connect and capture the run.
    std::printf("press any key to exit ...\n");
    std::fflush(stdout);
    wait_for_any_key();

#endif

    return EXIT_SUCCESS;
}

#if defined(P8_ENABLE_TRACY)
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Block until the user presses any key. Windows uses _getch(); POSIX puts the
// terminal into raw, no-echo mode for a single read() so no Enter is required.
void wait_for_any_key()
{
    #if defined(_WIN32)
    (void)_getch();
    #else
    struct termios lo_old = {};
    if(tcgetattr(STDIN_FILENO, &lo_old) != 0)
    {
        // Not a tty (e.g. piped input): fall back to a blocking line read.
        (void)std::getchar();
        return;
    }

    struct termios lo_raw  = lo_old;
    lo_raw.c_lflag        &= static_cast<tcflag_t>(~(ICANON | ECHO));
    lo_raw.c_cc[VMIN]      = 1;
    lo_raw.c_cc[VTIME]     = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &lo_raw);

    char lc_ch = 0;
    (void)read(STDIN_FILENO, &lc_ch, 1);

    tcsetattr(STDIN_FILENO, TCSANOW, &lo_old);
    #endif
}
#endif
