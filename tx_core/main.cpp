#include <uhd/exception.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/thread.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cmath>
#include <csignal>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

#define AMP_GPIO_MASK 0x00
#define MAN_GPIO_MASK 0xFF
#define ATR_MASKS (AMP_GPIO_MASK | MAN_GPIO_MASK)
#define ATR_CONTROL (AMP_GPIO_MASK)
#define GPIO_DDR (AMP_GPIO_MASK | MAN_GPIO_MASK)

namespace po = boost::program_options;
namespace spd = spdlog;

const double kTransmitSpan = .1;

static bool stop_signal_called = false;
void SigIntHandler(int) {
  stop_signal_called = true;
}

void GpioWorker(const uhd::usrp::multi_usrp::sptr &usrp, double rate, size_t num_samps) {
  // basic ATR configuration
  usrp->set_gpio_attr("FP0", "CTRL", ATR_CONTROL, ATR_MASKS);
  usrp->set_gpio_attr("FP0", "DDR", GPIO_DDR, ATR_MASKS);
  spd::info("num_samps: {}", num_samps);

  // start GPIO control loop
  unsigned int gpio_state;
  while (true) {
    auto command_time = std::ceil(usrp->get_time_now().get_real_secs() * 5) / 5;
//    for (int i = 0; i < std::ceil(kTransmitSpan / (static_cast<double>(num_samps) * 2 * 64 / rate)); i++) {
      for (int j = 0; j < 8; j++) {
        gpio_state = MAN_GPIO_MASK & ~(1 << j);
        usrp->set_command_time(uhd::time_spec_t(command_time));
        usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
        usrp->clear_command_time();
        command_time += (static_cast<double>(num_samps) * 16 / rate);
      }
//    }
    auto time_now = usrp->get_time_now().get_real_secs();
    // 0.2秒間隔で送信するため残りの時間待機
    auto delay_time = (std::ceil(time_now * 5) / 5 - time_now) * 1000;
//    spd::info("delay_time: {} ms", delay_time);
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay_time)));
    if (stop_signal_called) break;
  }
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
  // transmit variables to be set by po
  std::string args, file, ant, subdev, ref, pps, otw, channels;
  double rate, freq, gain, bw, lo_off;

  // initialize the logger
  spd::set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread %t] %v");

  // setup the program options
  po::options_description desc("Allowed options");
  desc.add_options()
      ("help", "help message")
      ("args", po::value<std::string>(&args)->default_value(""), "single uhd device address args")
      ("file", po::value<std::string>(&file)->default_value("signal.dat"), "name of the file to transmit")
      ("rate", po::value<double>(&rate), "rate of transmit outgoing samples")
      ("freq", po::value<double>(&freq), "RF center frequency in Hz")
      ("lo_off", po::value<double>(&lo_off)->default_value(-1), "Local oscillator offset")
      ("gain", po::value<double>(&gain), "gain for the RF chain")
      ("bw", po::value<double>(&bw), "analog frontend filter bandwidth in Hz")
      ("ant", po::value<std::string>(&ant), "antenna selection")
      ("subdev", po::value<std::string>(&subdev), "subdevice specification")
      ("ref",
       po::value<std::string>(&ref)->default_value("internal"),
       "reference source (internal, external, mimo)")
      ("pps", po::value<std::string>(&pps)->default_value("internal"), "PPS source (internal, external)")
      ("otw", po::value<std::string>(&otw)->default_value("sc16"), "specify the over-the-wire sample mode")
      ("channels", po::value<std::string>(&channels)->default_value("0"), "which channels to use");
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  // print the help message
  if (vm.count("help")) {
    std::cout << boost::format("UHD TX Samples From File %s") % desc << std::endl;
    return 0;
  }

  // create a usrp device
  spd::info("Creating the usrp device with: {}", args);
  uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);

  // always select the subdevice first, the channel mapping affects the other settings
  if (vm.count("subdev")) {
    spd::info("Subdev: {}", subdev);
    usrp->set_rx_subdev_spec(subdev);
  }
  spd::info("Using Device: {}", usrp->get_pp_string());

  // detect which channels to use
  std::vector<std::string> channel_strings;
  std::vector<size_t> channel_nums;
  boost::split(channel_strings, channels, boost::is_any_of("\"',"));
  for (const auto &kChannelString : channel_strings) {
    size_t chan = boost::lexical_cast<int>(kChannelString);
    if (chan >= usrp->get_tx_num_channels()) {
      throw std::runtime_error("Invalid channel(s) specified.");
    } else {
      channel_nums.push_back(boost::lexical_cast<int>(kChannelString));
    }
  }

  // lock mboard clocks
  spd::info("Lock mboard clocks");
  usrp->set_clock_source(ref);
  usrp->set_time_source(pps);
  // wait for mboard to lock
  spd::info("Waiting for clock lock...");
  while (not usrp->get_mboard_sensor("ref_locked").to_bool()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // pps edge 検出
  spd::info("Waiting for pps...");
  const uhd::time_spec_t kLastPpsTime = usrp->get_time_last_pps();
  while (usrp->get_time_last_pps() == kLastPpsTime) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  spd::info("PPS detected...");

  // set the sample rate
  if (not vm.count("rate")) {
    std::cerr << "Please specify the sample rate with --rate" << std::endl;
    return ~0;
  }
  spd::info("Setting sample rate: {} Msps", rate / 1e6);
  usrp->set_tx_rate(rate);
  spdlog::info("Actual TX Rate: {} Msps...", usrp->get_tx_rate() / 1e6);

  if (not vm.count("freq")) {
    std::cerr << "Please specify the center frequency with --freq" << std::endl;
    return ~0;
  }
  for (size_t chan : channel_nums) {
    spdlog::info("Configuring channel {}", chan);

    // set the center frequency
    spdlog::info("Setting TX Freq: {} MHz", freq / 1e6);
    uhd::tune_request_t tune_request(freq, lo_off);
    usrp->set_tx_freq(tune_request, chan);
    spdlog::info("Actual TX Freq: {} MHz...", usrp->get_tx_freq() / 1e6);


    // set the rf gain
    if (vm.count("gain")) {
      spdlog::info("Setting TX Gain: {} dB", gain);
      usrp->set_tx_gain(gain, chan);
      spdlog::info("Actual TX Gain: {} dB...", usrp->get_tx_gain());
    }

    // set the analog frontend filter bandwidth
    if (vm.count("bw")) {
      spdlog::info("Setting TX Bandwidth: {} MHz", bw / 1e6);
      usrp->set_tx_bandwidth(bw, chan);
      spdlog::info("Actual TX Bandwidth: {} MHz...", usrp->get_tx_bandwidth() / 1e6);
    }

    // set the antenna
    if (vm.count("ant")) {
      spdlog::info("Setting TX Antenna: {}", ant);
      usrp->set_tx_antenna(ant, chan);
      spdlog::info("Actual TX Antenna: {}", usrp->get_tx_antenna());
    }
  }

  // create a transmit streamer
  spdlog::info("Creating TX streamer...");
  uhd::stream_args_t stream_args("fc32", otw);
  stream_args.channels = channel_nums;

  // load send buffer from file
  spdlog::info("Reading in file: {}", file);
  std::ifstream infile(file.c_str(), std::ifstream::binary);
  if (!infile.good()) {
    spdlog::error("Could not open file: {}", file);
    return ~0;
  }
  infile.seekg(0, std::ifstream::end);
  size_t num_samps = infile.tellg() / sizeof(std::complex<float>);
  infile.seekg(0, std::ifstream::beg);
  std::vector<std::complex<float>> buff(num_samps);
  infile.read((char *) &buff.front(), static_cast<std::streamsize>(num_samps * sizeof(std::complex<float>)));
  infile.close();
  spdlog::info("num_samps: {}", num_samps);

  // check Ref and LO Lock detect
  // wait for LO lock
  spdlog::info("Waiting for TX lock (1 second)");
  std::this_thread::sleep_for(std::chrono::seconds(1)); //allow for some setup time
  std::vector<std::string> sensor_names;
  sensor_names = usrp->get_tx_sensor_names(0);
  if (std::find(sensor_names.begin(), sensor_names.end(), "lo_locked") != sensor_names.end()) {
    uhd::sensor_value_t lo_locked = usrp->get_tx_sensor("lo_locked", 0);
    spdlog::info("Checking TX: {}", lo_locked.to_pp_string());
    UHD_ASSERT_THROW(lo_locked.to_bool())
  }
  sensor_names = usrp->get_mboard_sensor_names(0);
  if ((ref == "external")
      and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end())) {
    uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked", 0);
    spdlog::info("Checking TX: {}", ref_locked.to_pp_string());
    UHD_ASSERT_THROW(ref_locked.to_bool())
  }

  std::signal(SIGINT, &SigIntHandler); // register ctrl-c handler
  spdlog::info("Press Ctrl + C to stop streaming...");

  spdlog::info("Setting device timestamp to 0...");
  usrp->set_time_next_pps(uhd::time_spec_t(0.0));
  std::this_thread::sleep_for(std::chrono::seconds(1)); //allow for some setup time


  uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(stream_args);
  auto max_num_samps = tx_stream->get_max_num_samps();
  spdlog::info("max_num_samps: {}", max_num_samps);
  if (max_num_samps > num_samps) max_num_samps = num_samps;

  // start gpio thread
  std::thread gpio_thread([&]() {
    GpioWorker(usrp, rate, 256);
  });

  usrp->clear_command_time();
  double send_time = std::ceil(usrp->get_time_now().get_real_secs());
  while (true) {

    uhd::tx_metadata_t md;
    md.time_spec = uhd::time_spec_t(send_time);
    md.start_of_burst = true;
    md.has_time_spec = true;

    double timeout = 1.5; //1.5
//	spdlog::info("Current time / Send time: {} / {}", time_now.get_real_secs(), md.time_spec.get_real_secs());
//	spdlog::info("Timeout: {}", timeout);
    spd::info("Send Time: {}", send_time);
//	usrp->clear_command_time();
//	spdlog::info("Last PPS time: {}", usrp->get_time_last_pps().get_real_secs());

//    auto num_send = static_cast<size_t>(std::ceil(rate / static_cast<double>(max_num_samps) * kTransmitSpan));
    auto num_send = 65;//19
    for (size_t i = 0; i < num_send; i++) {
      size_t num_sent = tx_stream->send(&buff.front(), max_num_samps, md, timeout);
      if (num_sent < max_num_samps) {
        spdlog::error("Sent {} / {} samples", num_sent, max_num_samps);
      }
      md.has_time_spec = false;
      md.start_of_burst = false;
    }

    // send a mini EOB packet
    md.end_of_burst = true;
    tx_stream->send("", 0, md);
    spdlog::info("Waiting for async burst ACK...");
    uhd::async_metadata_t async_md;
    bool got_async_burst_ack = false;
    // loop through all messages for the ACK packet (may have underflow messages in queue)
    while (not got_async_burst_ack and tx_stream->recv_async_msg(async_md, timeout)) {
      got_async_burst_ack = async_md.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK;
    }
    spdlog::info("Result: {}", (got_async_burst_ack ? "success" : "failure"));

    send_time += 0.200;
    if (stop_signal_called) break;
  }

  gpio_thread.join();
  spdlog::info("Done!");

  return EXIT_SUCCESS;
}

