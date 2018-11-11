//
// Copyright 2010-2011,2014 Ettus Research LLC
// Copyright 2018 Ettus Research, a National Instruments Company
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
//Modified by Nicole Bienert

#include <uhd/types/tune_request.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/exception.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <iostream>
#include <fstream>
#include <csignal>
#include <complex>
#include <thread>
#include <chrono>

namespace po = boost::program_options;

static bool stop_signal_called = false;
void sig_int_handler(int){stop_signal_called = true;}

/*  recv_to_file function has been modified to send data through udp rather than write to a file
	The function sets up a data stream then loops over putting samples in a buffer and giving 
	the buffer to a function which sends data over ethernet via udp
*/

template<typename samp_type> void recv_to_file(
	// variables passed into the function
    uhd::usrp::multi_usrp::sptr usrp,
    const std::string &cpu_format,
    const std::string &wire_format,
    const std::string &channel,
    const std::string &file,
    size_t samps_per_buff,
    unsigned long long num_requested_samples,
    const std::string &address, //I added this: this is the address of the server (my computer)
    const std::string &myPort, //I added this: the UDP port number
    double time_requested = 0.0,
    bool bw_summary = false,
    bool stats = false,
    bool null = false,
    bool enable_size_map = false,
    bool continue_on_bad_packet = false
){
 
	unsigned long long num_total_samps = 0;
    
	//create a receive streamer
    uhd::stream_args_t stream_args(cpu_format,wire_format);
    std::vector<size_t> channel_nums;
    channel_nums.push_back(boost::lexical_cast<size_t>(channel));
    stream_args.channels = channel_nums;
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    uhd::rx_metadata_t md;
    std::vector<samp_type> buff(samps_per_buff); //create a buffer
    std::ofstream outfile;

    bool overflow_message = true; //initialize overflow error

    //setup streaming
    uhd::stream_cmd_t stream_cmd((num_requested_samples == 0)?
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
    );
    stream_cmd.num_samps = size_t(num_requested_samples);
    stream_cmd.stream_now = true;
    stream_cmd.time_spec = uhd::time_spec_t();
    rx_stream->issue_stream_cmd(stream_cmd); //start streaming

	//set up timing variables
    boost::system_time start = boost::get_system_time();
    unsigned long long ticks_requested = (long)(time_requested * (double)boost::posix_time::time_duration::ticks_per_second());
    boost::posix_time::time_duration ticks_diff;
    boost::system_time last_update = start;
    unsigned long long last_update_samps = 0;

	/////////////////////////////////// Nicole Addition ///////////////////////////////////////////////
	// connect via udp to the server
	uhd::transport::udp_simple::sptr udp_xport = uhd::transport::udp_simple::make_connected(address,myPort);
	///////////////////////////////////////////////////////////////////////////////////////////////////

    typedef std::map<size_t,size_t> SizeMap;
    SizeMap mapSizes;

	//loop which grabs data and sends it over udp
    while(not stop_signal_called and (num_requested_samples != num_total_samps or num_requested_samples == 0)) { //loop until desired num samples reached
        boost::system_time now = boost::get_system_time(); //set loop start time used for calculating sample rate

        size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0, enable_size_map); //record samples and stick them in a buffer

		//Throw an error if recv passed an error
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << boost::format("Timeout while streaming") << std::endl;
            break;
        }
		//Overflow error
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW){ //this is the primary error I get which I am trying to overcome
            if (overflow_message) {
                overflow_message = false;
                std::cerr << boost::format(
                    "Got an overflow indication. Please consider the following:\n"
                    "  Your write medium must sustain a rate of %fMB/s.\n"
                    "  Dropped samples will not be written to the file.\n"
                    "  Please modify this example for your purposes.\n"
                    "  This message will not appear again.\n"
                ) % (usrp->get_rx_rate()*sizeof(samp_type)/1e6);
            }
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE){
            std::string error = str(boost::format("Receiver error: %s") % md.strerror());
            if (continue_on_bad_packet){
                std::cerr << error << std::endl;
                continue;
            }
            else
                throw std::runtime_error(error);
        }

        if (enable_size_map) {
            SizeMap::iterator it = mapSizes.find(num_rx_samps);
            if (it == mapSizes.end())
                mapSizes[num_rx_samps] = 0;
            mapSizes[num_rx_samps] += 1;
        }

        num_total_samps += num_rx_samps; //increase the total samples recorded

		///////////////////////////////////Nicole Addition ///////////////////////////////////////////////////////////
		//write through udp
		int sent =	udp_xport->send(boost::asio::buffer((const char*)&buff.front(), num_rx_samps*sizeof(samp_type)));
		// this also works: udp_xport->send(boost::asio::buffer(buff, num_rx_samps*sizeof(buff.front()))); 
		//std::cout << "num bytes sent=";
		//std::cout << sent;
		//////////////////////////////////////////////////////////////////////////////////////////////////////////////

		//display an update of the current samp/s rate
        if (bw_summary) {
            last_update_samps += num_rx_samps;
            boost::posix_time::time_duration update_diff = now - last_update;
            if (update_diff.ticks() > boost::posix_time::time_duration::ticks_per_second()) {
                double t = (double)update_diff.ticks() / (double)boost::posix_time::time_duration::ticks_per_second();
                double r = (double)last_update_samps / t;
                std::cout << boost::format("\t%f Msps") % (r/1e6) << std::endl;
                last_update_samps = 0;
                last_update = now;
            }
        }

        ticks_diff = now - start;
        if (ticks_requested > 0){
            if ((unsigned long long)ticks_diff.ticks() > ticks_requested)
                break;
        }
    }

	//if loop has exited, stop streaming
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS; //set stream command to stop
    rx_stream->issue_stream_cmd(stream_cmd); //send stream command

	//if requested, report the overall sample rate and total samples recorded
    if (stats) {
        std::cout << std::endl;

        double t = (double)ticks_diff.ticks() / (double)boost::posix_time::time_duration::ticks_per_second();
        std::cout << boost::format("Received %d samples in %f seconds") % num_total_samps % t << std::endl;
        double r = (double)num_total_samps / t;
        std::cout << boost::format("%f Msps") % (r/1e6) << std::endl;

        if (enable_size_map) {
            std::cout << std::endl;
            std::cout << "Packet size map (bytes: count)" << std::endl;
            for (SizeMap::iterator it = mapSizes.begin(); it != mapSizes.end(); it++)
                std::cout << it->first << ":\t" << it->second << std::endl;
        }
    }
}




typedef boost::function<uhd::sensor_value_t (const std::string&)> get_sensor_fn_t;
// function to lock to LO
bool check_locked_sensor(std::vector<std::string> sensor_names, const char* sensor_name, get_sensor_fn_t get_sensor_fn, double setup_time){
    if (std::find(sensor_names.begin(), sensor_names.end(), sensor_name) == sensor_names.end())
        return false;

    boost::system_time start = boost::get_system_time();
    boost::system_time first_lock_time;

    std::cout << boost::format("Waiting for \"%s\": ") % sensor_name;
    std::cout.flush();

    while (true) {
        if ((not first_lock_time.is_not_a_date_time()) and
                (boost::get_system_time() > (first_lock_time + boost::posix_time::seconds(setup_time))))
        {
            std::cout << " locked." << std::endl;
            break;
        }
        if (get_sensor_fn(sensor_name).to_bool()){
            if (first_lock_time.is_not_a_date_time())
                first_lock_time = boost::get_system_time();
            std::cout << "+";
            std::cout.flush();
        }
        else {
            first_lock_time = boost::system_time(); //reset to 'not a date time'

            if (boost::get_system_time() > (start + boost::posix_time::seconds(setup_time))){
                std::cout << std::endl;
                throw std::runtime_error(str(boost::format("timed out waiting for consecutive locks on sensor \"%s\"") % sensor_name));
            }
            std::cout << "_";
            std::cout.flush();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << std::endl;
    return true;
}


// Main code
int UHD_SAFE_MAIN(int argc, char *argv[]){
    uhd::set_thread_priority_safe();

    //variables to be set by po (program options)
    std::string args, file, type, ant, subdev, ref, wirefmt, channel;
    std::string addr, port;
    size_t total_num_samps, spb;
    double rate, freq, gain, bw, total_time, setup_time;

    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&args)->default_value(""), "multi uhd device address args")
        ("file", po::value<std::string>(&file)->default_value("usrp_samples.dat"), "name of the file to write binary samples to")
        ("type", po::value<std::string>(&type)->default_value("short"), "sample type: double, float, or short")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "total number of samples to receive")
        ("duration", po::value<double>(&total_time)->default_value(0), "total number of seconds to receive")
        ("time", po::value<double>(&total_time), "(DEPRECATED) will go away soon! Use --duration instead")
        ("spb", po::value<size_t>(&spb)->default_value(10000), "samples per buffer")
        ("rate", po::value<double>(&rate)->default_value(1e6), "rate of incoming samples")
        ("freq", po::value<double>(&freq)->default_value(0.0), "RF center frequency in Hz")
        ("gain", po::value<double>(&gain), "gain for the RF chain")
        ("ant", po::value<std::string>(&ant), "antenna selection")
        ("subdev", po::value<std::string>(&subdev), "subdevice specification")
        ("channel", po::value<std::string>(&channel)->default_value("0"), "which channel to use")
        ("bw", po::value<double>(&bw), "analog frontend filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "reference source (internal, external, mimo)")
        ("wirefmt", po::value<std::string>(&wirefmt)->default_value("sc16"), "wire format (sc8, sc16 or s16)")
        ("setup", po::value<double>(&setup_time)->default_value(1.0), "seconds of setup time")
        ("progress", "periodically display short-term bandwidth")
        ("stats", "show average bandwidth on exit")
        ("sizemap", "track packet size and display breakdown on exit")
        ("null", "run without writing to file")
        ("continue", "don't abort on a bad packet")
        ("skip-lo", "skip checking LO lock status")
        ("int-n", "tune USRP with integer-N tuning")
		("addr",po::value<std::string>(&addr)->default_value("169.254.234.25"), "resolvable server address") // variables I added
		("port",po::value<std::string>(&port)->default_value("2222"),"server udp port") //variables I added

    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")) {
        std::cout << boost::format("UHD RX samples to file %s") % desc << std::endl;
        std::cout
            << std::endl
            << "This application streams data from a single channel of a USRP device to a file.\n"
            << std::endl;
        return ~0;
    }

    bool bw_summary = vm.count("progress") > 0;
    bool stats = vm.count("stats") > 0;
    bool null = vm.count("null") > 0;
    bool enable_size_map = vm.count("sizemap") > 0;
    bool continue_on_bad_packet = vm.count("continue") > 0;

    if (enable_size_map)
        std::cout << "Packet size tracking enabled - will only recv one packet at a time!" << std::endl;

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the usrp device with: %s...") % args << std::endl;
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);

    //Lock mboard clocks
    usrp->set_clock_source(ref);

    //always select the subdevice first, the channel mapping affects the other settings
    if (vm.count("subdev")) usrp->set_rx_subdev_spec(subdev);

    std::cout << boost::format("Using Device: %s") % usrp->get_pp_string() << std::endl;

    //set the sample rate
    if (rate <= 0.0){
        std::cerr << "Please specify a valid sample rate" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rate/1e6) << std::endl;
    usrp->set_rx_rate(rate);
    std::cout << boost::format("Actual RX Rate: %f Msps...") % (usrp->get_rx_rate()/1e6) << std::endl << std::endl;

    //set the center frequency
    if (vm.count("freq")) { //with default of 0.0 this will always be true
        std::cout << boost::format("Setting RX Freq: %f MHz...") % (freq/1e6) << std::endl;
        uhd::tune_request_t tune_request(freq);
        if(vm.count("int-n")) tune_request.args = uhd::device_addr_t("mode_n=integer");
        usrp->set_rx_freq(tune_request);
        std::cout << boost::format("Actual RX Freq: %f MHz...") % (usrp->get_rx_freq()/1e6) << std::endl << std::endl;
    }

    //set the rf gain
    if (vm.count("gain")) {
        std::cout << boost::format("Setting RX Gain: %f dB...") % gain << std::endl;
        usrp->set_rx_gain(gain);
        std::cout << boost::format("Actual RX Gain: %f dB...") % usrp->get_rx_gain() << std::endl << std::endl;
    }

    //set the IF filter bandwidth
    if (vm.count("bw")) {
        std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % (bw/1e6) << std::endl;
        usrp->set_rx_bandwidth(bw);
        std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % (usrp->get_rx_bandwidth()/1e6) << std::endl << std::endl;
    }

    //set the antenna
    if (vm.count("ant")) usrp->set_rx_antenna(ant);

    std::this_thread::sleep_for(
        std::chrono::milliseconds(int64_t(1000 * setup_time))
    );

    //check Ref and LO Lock detect
    if (not vm.count("skip-lo")){
        check_locked_sensor(usrp->get_rx_sensor_names(0), "lo_locked", boost::bind(&uhd::usrp::multi_usrp::get_rx_sensor, usrp, _1, 0), setup_time);
        if (ref == "mimo")
            check_locked_sensor(usrp->get_mboard_sensor_names(0), "mimo_locked", boost::bind(&uhd::usrp::multi_usrp::get_mboard_sensor, usrp, _1, 0), setup_time);
        if (ref == "external")
            check_locked_sensor(usrp->get_mboard_sensor_names(0), "ref_locked", boost::bind(&uhd::usrp::multi_usrp::get_mboard_sensor, usrp, _1, 0), setup_time);
    }

    if (total_num_samps == 0){
        std::signal(SIGINT, &sig_int_handler);
        std::cout << "Press Ctrl + C to stop streaming..." << std::endl;
    }
	
	//weird macro thing which sends the variables to the recv_to_file function
#define recv_to_udp_nicole_args(format) \
    (usrp, format, wirefmt, channel, file, spb, total_num_samps, addr, port, total_time, bw_summary, stats, null, enable_size_map, continue_on_bad_packet)
    
	//Call recv_to_file which records data and sends it over ethernet
    if (wirefmt == "s16") {
        if (type == "double") recv_to_file<double>recv_to_udp_nicole_args("f64");
        else if (type == "float") recv_to_file<float>recv_to_udp_nicole_args("f32");
        else if (type == "short") recv_to_file<short>recv_to_udp_nicole_args("s16");
        else throw std::runtime_error("Unknown type " + type);
    } else {
        if (type == "double") recv_to_file<std::complex<double> >recv_to_udp_nicole_args("fc64");
        else if (type == "float") recv_to_file<std::complex<float> >recv_to_udp_nicole_args("fc32");
        else if (type == "short") recv_to_file<std::complex<short> >recv_to_udp_nicole_args("sc16");
        else throw std::runtime_error("Unknown type " + type);
    }

    //finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;

    return EXIT_SUCCESS;
}
