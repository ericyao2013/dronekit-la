#include "loganalyzer.h"

#include "mavlink_reader.h"

#include "heart.h"

#include <syslog.h>
#include "la-log.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <regex>

#include "dataflash_reader.h"

#include "analyzing_dataflash_message_handler.h"
#include "analyzing_mavlink_message_handler.h"

void LogAnalyzer::parse_path(const char *path)
{
    create_vehicle_from_commandline_arguments();
    if (_vehicle == NULL) {
        _vehicle = new AnalyzerVehicle::Base();
    }

    bool is_dataflash_log = false;
    bool do_stdin = false;

    if (!strcmp(path, "-")) {
        do_stdin = true;
    } else if (strstr(path, ".BIN") ||
               strstr(path, ".bin")) {
        is_dataflash_log = true;
    }

    switch (force_format()) {
    case log_format_tlog:
        break;
    case log_format_df:
        is_dataflash_log = true;
        break;
    default:
        break;
    }
    
    if (do_stdin && force_format() == log_format_none) {
        ::fprintf(stderr, "You asked to parse stdin but did not force a format type\n");
        usage();
        exit(1);
    }

    if (is_dataflash_log) {
        prep_for_df();
    } else {
        prep_for_tlog();
    }

    int fd;
    if (!strcmp(path, "-")) {
        fd = fileno(stdin);
    } else {
        fd = xopen(path, O_RDONLY);
    }

    if (output_style == Analyze::OUTPUT_BRIEF) {
        printf("%25s: ", path);
    }

    parse_fd(reader, fd);

    if (output_style == Analyze::OUTPUT_BRIEF) {
        printf("\n");
    }

    if (is_dataflash_log) {
        cleanup_after_df();
    } else {
        cleanup_after_tlog();
    }
    
    delete _vehicle;
    _vehicle = NULL;
}

int LogAnalyzer::xopen(const char *filepath, const uint8_t mode)
{
    int fd = open(filepath, mode);
    if (fd == -1) {
        fprintf(stderr, "Failed to open (%s): %s\n", filepath, strerror(errno));
        exit(1);
    }
    return fd;
}

void LogAnalyzer::prep_for_tlog()
{
    reader = new MAVLink_Reader(config());
    ((MAVLink_Reader*)reader)->set_is_tlog(true);

    Analyze *analyze = create_analyze();

    Analyzing_MAVLink_Message_Handler *handler = new Analyzing_MAVLink_Message_Handler(analyze, _vehicle);
    reader->add_message_handler(handler, "Analyze");
}

void LogAnalyzer::cleanup_after_tlog()
{
    reader->clear_message_handlers();
}
void LogAnalyzer::cleanup_after_df()
{
    reader->clear_message_handlers();
}

void LogAnalyzer::do_idle_callbacks()
{
    reader->do_idle_callbacks();
}
void LogAnalyzer::pack_select_fds(fd_set &fds_read, fd_set &fds_write, fd_set &fds_err, uint8_t &nfds)
{
    _client->pack_select_fds(fds_read, fds_write, fds_err, nfds);
}

void LogAnalyzer::handle_select_fds(fd_set &fds_read, fd_set &fds_write, fd_set &fds_err, uint8_t &nfds)
{
    _client->handle_select_fds(fds_read, fds_write, fds_err, nfds);

    // FIXME: find a more interesting way of doing this...
    reader->feed(_client_buf, _client->_buflen_content);
    _client->_buflen_content = 0;
}

void LogAnalyzer::run_live_analysis()
{
    reader = new MAVLink_Reader(config());

    _client = new Telem_Forwarder_Client(_client_buf, sizeof(_client_buf));
    _client->configure(config());

    Heart *heart= new Heart(_client->fd_telem_forwarder, &_client->sa_tf);
    if (heart != NULL) {
        reader->add_message_handler(heart, "Heart");
    } else {
        la_log(LOG_INFO, "Failed to create heart");
    }

    create_vehicle_from_commandline_arguments();
    if (_vehicle == NULL) {
        _vehicle = new AnalyzerVehicle::Base();
    }

    Analyze *analyze = create_analyze();

    Analyzing_MAVLink_Message_Handler *handler = new Analyzing_MAVLink_Message_Handler(analyze, _vehicle);
    reader->add_message_handler(handler, "Analyze");

    select_loop();

    _vehicle = NULL; // leak this memory
}

Analyze *LogAnalyzer::create_analyze()
{
    Analyze *analyze = new Analyze(_vehicle);
    if (analyze == NULL) {
        la_log(LOG_ERR, "Failed to create analyze");
        abort();
    }

    analyze->set_output_style(output_style);
    if (_analyzer_names_to_run.size()) {
        analyze->set_analyzer_names_to_run(_analyzer_names_to_run);
    }
    analyze->instantiate_analyzers(config());

    return analyze;
}

void LogAnalyzer::prep_for_df()
{
    reader = new DataFlash_Reader(config());

    Analyze *analyze = create_analyze();

    Analyzing_DataFlash_Message_Handler *handler = new Analyzing_DataFlash_Message_Handler(analyze, _vehicle);
    reader->add_message_handler(handler, "Analyze");
}

void LogAnalyzer::show_version_information()
{
    ::printf("Version: " GIT_VERSION "\n");
}

void LogAnalyzer::list_analyzers()
{
    Analyze *analyze = new Analyze(_vehicle);

    analyze->instantiate_analyzers(config());
    std::vector<Analyzer *> analyzers = analyze->analyzers();
    for (std::vector<Analyzer*>::iterator it = analyzers.begin();
         it != analyzers.end();
         ++it) {
        ::printf("%s\n", (*it)->name().c_str());
    }
}

// From: http://stackoverflow.com/questions/236129/split-a-string-in-c
std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}
std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}
// thanks stackoverflow!

void LogAnalyzer::expand_names_to_run()
{
    std::vector<std::string> new_names;
    for (std::vector<std::string>::const_iterator it = _analyzer_names_to_run.begin();
         it != _analyzer_names_to_run.end();
         ++it) {
        // insert lambda here if you dare.
        std::vector<std::string> tmp = split((*it),',');
        
        for (std::vector<std::string>::const_iterator iy = tmp.begin();
             iy != tmp.end();
             ++iy) {
            std::string value = std::regex_replace(*iy, std::regex("^ +| +$|( ) +"), "$1");
            new_names.push_back(value);
        }
    }
    _analyzer_names_to_run = new_names;
}

void LogAnalyzer::create_vehicle_from_commandline_arguments()
{
    if (_model_string != NULL) {
        if (streq(_model_string,"copter")) {
            _vehicle = new AnalyzerVehicle::Copter();
//            _analyze->set_vehicle_copter();
            if (_frame_string != NULL) {
                ((AnalyzerVehicle::Copter*)_vehicle)->set_frame(_frame_string);
            }
        // } else if (streq(model_string,"plane")) {
        //     model = new AnalyzerVehicle::Plane();
        // } else if (streq(model_string,"rover")) {
        //     model = new AnalyzerVehicle::Rover();
        } else {
            la_log(LOG_ERR, "Unknown model type (%s)", _model_string);
            exit(1);
        }
    }
}

void LogAnalyzer::run()
{
    // la_log(LOG_INFO, "loganalyzer starting: built " __DATE__ " " __TIME__);
    // signal(SIGHUP, sighup_handler);
    init_config();

    if (_show_version_information) {
        show_version_information();
        exit(0);
    }
    if (_do_list_analyzers) {
        list_analyzers();
        exit(0);
    }

    expand_names_to_run();

    if (_use_telem_forwarder) {
        return run_live_analysis();
    }

    if (_paths == NULL) {
        usage();
        exit(1);
    }

    output_style = Analyze::OUTPUT_JSON;
    if (output_style_string != NULL) {
        output_style = Analyze::OUTPUT_JSON;
        if (streq(output_style_string, "json")) {
            output_style = Analyze::OUTPUT_JSON;
        } else if(streq(output_style_string, "plain-text")) {
            output_style = Analyze::OUTPUT_PLAINTEXT;
        } else if(streq(output_style_string, "brief")) {
            output_style = Analyze::OUTPUT_BRIEF;
        } else if(streq(output_style_string, "html")) {
            output_style = Analyze::OUTPUT_HTML;
        } else {
            usage();
            exit(1);
        }
    }

    if (forced_format_string != NULL) {
        if (streq(forced_format_string, "tlog")) {
            _force_format = log_format_tlog;
        } else if(streq(forced_format_string, "df")) {
            _force_format = log_format_df;
        } else {
            usage();
            exit(1);
        }
    }

    for (uint8_t i=0; i<_pathcount; i++) {
        parse_path(_paths[i]);
    }
}

void LogAnalyzer::usage()
{
    ::printf("Usage:\n");
    ::printf("%s [OPTION] [FILE...]\n", program_name());
    ::printf(" -c filepath      use config file filepath\n");
    ::printf(" -t               connect to telem forwarder to receive data\n");
    ::printf(" -m modeltype     override model; copter|plane|rover\n");
    ::printf(" -f frame         set frame; QUAD|Y6\n");
    ::printf(" -s style         use output style (plain-text|json|brief)\n");
    ::printf(" -h               display usage information\n");
    ::printf(" -l               list analyzers\n");
    ::printf(" -a               specify analzers to run (comma-separated list)\n");
    ::printf(" -i format        specify format (tlog|df)\n");
    ::printf(" -V               display version information\n");
    ::printf("\n");
    ::printf("Example: %s -s json 1.solo.tlog\n", program_name());
    ::printf("Example: %s -a \"Ever Flew, Battery\" 1.solo.tlog\n", program_name());
    ::printf("Example: %s -s brief 1.solo.tlog 2.solo.tlog logs/*.tlog\n", program_name());
    ::printf("Example: %s - (analyze stdin)\n", program_name());
    exit(0);
}
const char *LogAnalyzer::program_name()
{
    if (_argv == NULL) {
        return "[Unknown]";
    }
    return _argv[0];
}


void LogAnalyzer::parse_arguments(int argc, char *argv[])
{
    int opt;
    _argc = argc;
    _argv = argv;

    while ((opt = getopt(argc, argv, "hc:ts:m:f:Vla:i:")) != -1) {
        switch(opt) {
        case 'h':
            usage();
            break;
        case 'c':
            config_filename = optarg;
            break;
        case 'i':
            forced_format_string = optarg;
            break;
        case 't':
            _use_telem_forwarder = true;
            break;
        case 's':
            output_style_string = optarg;
            break;
        case 'm':
            _model_string = optarg;
            break;
        case 'f':
            _frame_string = optarg;
            break;
        case 'V':
            _show_version_information = true;
            break;
        case 'l':
            _do_list_analyzers = true;
            break;
        case 'a':
            _analyzer_names_to_run.push_back(optarg);
            break;
        }
    }
    if (optind < argc) {
        _paths = &argv[optind];
        _pathcount = argc-optind;
    }
}

/*
* main - entry point
*/
int main(int argc, char* argv[])
{
    LogAnalyzer analyzer;
    analyzer.parse_arguments(argc, argv);
    analyzer.run();
    exit(0);
}
