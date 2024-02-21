#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedMacroInspection"

#include <uhd/exception.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <complex>
#include <cmath>
#include <csignal>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <boost/asio.hpp>

// GPIO pin config
#define AMP_GPIO_MASK 0x00
#define MAN_GPIO_MASK 0xFF
#define ATR_MASKS (AMP_GPIO_MASK | MAN_GPIO_MASK)
#define ATR_CONTROL (AMP_GPIO_MASK)
#define GPIO_DDR (AMP_GPIO_MASK | MAN_GPIO_MASK)

namespace po = boost::program_options;

std::atomic<bool> keep_transmitting{false};

static bool stop_signal_called = false;
#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCDFAInspection"
void SigIntHandler(const boost::system::error_code &error, int signal_number, boost::asio::io_context *io_context) {
  if (signal_number == SIGINT) {
    stop_signal_called = true;
    keep_transmitting = false;
    io_context->stop();
  }
}
#pragma clang diagnostic pop

void TransmitWorker(boost::asio::ip::tcp::socket &socket, const uhd::tx_streamer::sptr &tx_stream,
                    const std::vector<std::complex<float>> &tx_buff, size_t tx_file_num_samps,
                    size_t total_num_samps, size_t max_num_samps, double stream_time) {
  boost::asio::write(socket, boost::asio::buffer("1", 1)); // 送信開始通知
  while (keep_transmitting) {
    uhd::tx_metadata_t md;
    md.time_spec = uhd::time_spec_t(stream_time);
    md.start_of_burst = true;
    md.has_time_spec = true;

    double timeout = 1.5;   //todo: Too big?
    spdlog::info("Send Time: {}", stream_time);

    //todo: delayだけnull送る
    //todo: なぜ *10 で動く？ -> 要調査
    size_t num_send = std::ceil(total_num_samps / tx_file_num_samps * 10);//19 < 65
    spdlog::info("Send frames: {}", num_send);
    for (size_t i = 0; i < num_send; i++) {
      size_t num_sent = tx_stream->send(&tx_buff.front(), max_num_samps, md, timeout);
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

    stream_time += .200;
    if (stop_signal_called) break;
  }
  boost::asio::write(socket, boost::asio::buffer("2", 1)); // 送信停止通知
}

void GpioWorker(const uhd::usrp::multi_usrp::sptr &usrp,
                double rate,
                size_t num_samps,
                size_t tx_ports,
                size_t rx_ports,
                bool dir,    //Rx:0 Tx:1
                double base_time) {
  // basic ATR configuation
  usrp->set_gpio_attr("FP0", "CTRL", ATR_CONTROL, ATR_MASKS);
  usrp->set_gpio_attr("FP0", "DDR", GPIO_DDR, ATR_MASKS);

  //start GPIO control
  unsigned int gpio_state;
  auto command_time = base_time;

  if (dir == 0) {
    //rx
    for (int i = 0; i < tx_ports; i++) {
      for (int j = 0; j < rx_ports; j++) {
        gpio_state = MAN_GPIO_MASK & ~(1 << j);
        usrp->set_command_time(command_time);
        usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
        usrp->clear_command_time();
        command_time += (static_cast<double>(num_samps) * 2 / rate);
      }
    }
  } else {
    //tx
    while (keep_transmitting) {
      command_time = base_time;
      for (int j = 0; j < tx_ports; j++) {
        gpio_state = MAN_GPIO_MASK & ~(1 << j);
        usrp->set_command_time(command_time);
        usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
        usrp->clear_command_time();
        command_time += (static_cast<double>(num_samps) * 2 * static_cast<double>(rx_ports) / rate);
      }
      base_time += .200;
      auto time_now = usrp->get_time_now().get_real_secs();
      auto delay_time = (base_time - time_now) * 1e3 - 100;
      if (delay_time > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(delay_time)));
      }
      if (stop_signal_called) break;
    }
  }
  gpio_state = 0xFF;
  command_time += (static_cast<double>(num_samps) * 2 / rate);
  usrp->set_command_time(command_time);
  usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
  usrp->clear_command_time();
  spdlog::info("GPIO finished");
}

void SocketWorker(boost::asio::io_context &io_context, unsigned short port,
                  const uhd::usrp::multi_usrp::sptr &usrp,
                  const uhd::rx_streamer::sptr &rx_stream,
                  const uhd::tx_streamer::sptr &tx_stream,
                  const std::vector<std::complex<float>> &tx_buff,
                  const uhd::transport::udp_simple::sptr &udp_sock,
                  size_t tx_file_num_samps, size_t max_num_samps,
                  size_t num_delay, const std::string &rx_file, size_t rx_ports, size_t tx_ports,
                  double rate, size_t num_samps) {
  spdlog::info("Setting up TCP socket...");
  boost::asio::ip::tcp::acceptor acceptor(io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port));
  boost::asio::ip::tcp::socket socket(io_context);
  acceptor.accept(socket);
  spdlog::info("TCP Connected");
  boost::asio::write(socket, boost::asio::buffer("0")); // 接続完了通知

  std::thread gpio_thread;
  std::thread tx_thread;

  for (;;) {
    std::string message(10, '\0');
    boost::system::error_code error;
    size_t length = socket.read_some(boost::asio::buffer(&message[0], message.size()), error);
    if (error == boost::asio::error::eof) {
      spdlog::info("TCP Disconnected");
      break;
    } else if (error) {
      throw std::runtime_error(error.message());
    }
    message.resize(length);
    spdlog::info("TCP Received: {}", message);

    auto time_now = usrp->get_time_now().get_real_secs();
    auto stream_time = std::ceil(time_now * 5) / 5;
    if (stream_time < time_now + 0.05) {
      stream_time += 0.2;
    }

    auto total_num_samps = num_samps * tx_ports * rx_ports * 2 + num_delay;

    if (message == "1") {
      keep_transmitting = true;
      gpio_thread = std::thread([&]() {
        GpioWorker(usrp, rate, num_samps, tx_ports, rx_ports, true,
                   stream_time + static_cast<double>(num_delay) / rate);
      });
      tx_thread = std::thread([&]() {
        TransmitWorker(socket, tx_stream, tx_buff, tx_file_num_samps, total_num_samps, max_num_samps, stream_time);
      });
    } else if (message == "2") {
      keep_transmitting = false;
      spdlog::info("Stop Transmitting");
      if (gpio_thread.joinable()) gpio_thread.join();
      if (tx_thread.joinable()) tx_thread.join();
    } else if (message == "3") {
      //Rx
      // setup streaming
      gpio_thread = std::thread([&]() {
        GpioWorker(usrp, rate, num_samps, tx_ports, rx_ports, false,
                   stream_time + static_cast<double>(num_delay) / rate);
      });
      uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
      stream_cmd.num_samps = total_num_samps;
      stream_cmd.stream_now = false;
      stream_cmd.time_spec = uhd::time_spec_t(stream_time);
      rx_stream->issue_stream_cmd(stream_cmd);
      spdlog::info("Begin streaming {} samples at {}", total_num_samps, stream_time);

      // meta-data will be filled in by recv()
      uhd::rx_metadata_t md;

      // allocate buffer to receive with samples
      std::vector<std::complex<float>> rx_buff(rx_stream->get_max_num_samps());
      std::vector<std::complex<float>> rx_buffs;


      // the first call to recv() will block this many seconds before receiving
      double timeout = 0.5;

      size_t num_acc_samps = 0;
      while (num_acc_samps < total_num_samps) {

        // receive a single packet
        size_t num_rx_samps;
        try {
          num_rx_samps = rx_stream->recv(&rx_buff.front(), rx_buff.size(), md, timeout);
        } catch (uhd::io_error &e) {
          spdlog::error("Caught an IO exception: {}", e.what());
          break;
        }
        // use a small timeout for subsequent packetnumber of seconds in the future to receives
        timeout = 0.1;

        // handle the error code
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
          spdlog::error("Receiver error: {}", md.strerror());
          break;
        }

        num_acc_samps += num_rx_samps;
        rx_buffs.insert(rx_buffs.end(), rx_buff.begin(), rx_buff.end());
      }

      if (num_acc_samps < total_num_samps) {
        spdlog::warn("Did not receive all samples: {} out of {}", num_acc_samps, total_num_samps);
        rx_buff.clear();
        rx_buffs.clear();
        boost::asio::write(socket, boost::asio::buffer("4", 1)); // 受信失敗通知
      } else {
        rx_buffs.erase(rx_buffs.begin(), rx_buffs.begin() + static_cast<int>(num_delay));
        num_acc_samps -= num_delay;
        auto rcvd_time = usrp->get_time_now().get_real_secs();
        spdlog::info("Recieved {} samples at {}", num_acc_samps, rcvd_time);
        size_t type_size = sizeof(rx_buffs.front());
        while (num_acc_samps > 0) {
          size_t num_tx_samps;
          if (num_acc_samps < 2000) {
            udp_sock->send(boost::asio::buffer(rx_buffs, num_acc_samps * type_size));
            num_tx_samps = num_acc_samps;
          } else {
            udp_sock->send(boost::asio::buffer(rx_buffs, 2000 * type_size));
            num_tx_samps = 2000;
          }
          rx_buffs.erase(rx_buffs.begin(), rx_buffs.begin() + static_cast<int>(num_tx_samps));
          num_acc_samps -= num_tx_samps;
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        rx_buffs.clear();
        if (!rx_file.empty()) {
          std::ofstream outfile(rx_file, std::ofstream::binary);
          outfile.write((const char *) &rx_buffs.front(),
                        std::streamsize(num_acc_samps * sizeof(std::complex<float>)));
          outfile.close();
        }
        boost::asio::write(socket, boost::asio::buffer("3", 1)); // 受信完了通知
      }
      gpio_thread.join();
    }
  }
  keep_transmitting = false;
  if (gpio_thread.joinable()) gpio_thread.join();
  if (tx_thread.joinable()) tx_thread.join();
  io_context.stop();
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
int UHD_SAFE_MAIN(int argc, char *argv[]) {
#pragma clang diagnostic pop
  // variables to be set by po
  std::string args, subdev, ref, otw, channels, antenna, tx_ant, rx_file, file, addr, udp_port, tcp_port;
  size_t num_samps, rx_ports, tx_ports, num_delay;
  double rate, freq, rx_gain, tx_gain, bw, lo_off;
  bool use_tcp = false;

  // initialize the logger
  spdlog::set_level(spdlog::level::debug);
  spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] [thread %t] %v");
  spdlog::info("Starting");

  // setup the program options
  po::options_description desc("Allowed options");
  // clang-format off
  desc.add_options()
      ("help", "help message")
      ("args", po::value<std::string>(&args)->default_value(""),
       "single uhd device address args")
      ("rx-file", po::value<std::string>(&rx_file)->default_value(""), "file path to write to")
      ("tx-file", po::value<std::string>(&file)->default_value("signal.dat"), "name of the file to transmit")
      ("rate", po::value<double>(&rate), "rate of incoming samples")
      ("lo_off", po::value<double>(&lo_off)->default_value(-1),
       "offset from the center frequency")
      ("freq", po::value<double>(&freq), "RF center frequency in Hz")
      ("rx-gain", po::value<double>(&rx_gain), "gain for the Rx RF chain")
      ("tx-gain", po::value<double>(&tx_gain), "gain for the Tx RF chain")
      ("bw", po::value<double>(&bw), "analog frontend filter bandwidth in Hz")
      ("subdev", po::value<std::string>(&subdev), "subdevice specification")
      ("ref", po::value<std::string>(&ref)->default_value("internal"),
       "reference source (internal, external, mimo, gpsdo)")
      ("otw", po::value<std::string>(&otw)->default_value("sc16"),
       "specify the over-the-wire sample mode")
      ("channels", po::value<std::string>(&channels)->default_value("0"),
       "which channels to use")
      ("rx-ant", po::value<std::string>(&antenna)->default_value("TX/RX"),
       "which rx antenna to use (TX/RX, RX2, CAL)")
      ("tx-ant", po::value<std::string>(&tx_ant), "which tx antenna to use")
      ("samps",
       po::value<size_t>(&num_samps)->default_value(256),
       "total number of samples to receive")
      ("rx-ports", po::value<size_t>(&rx_ports)->default_value(8), "number of Rx ports")
      ("tx-ports", po::value<size_t>(&tx_ports)->default_value(8), "number of Tx ports")
      ("delay", po::value<size_t>(&num_delay)->default_value(0), "delay samples")
      ("addr", po::value<std::string>(&addr)->default_value("127.0.0.1"), "IP address")
      ("port", po::value<std::string>(&udp_port)->default_value("12345"), "port number")
      ("tcp-port", po::value<std::string>(&tcp_port)->default_value("54321"), "TCP port number")
      ("repeat", "if set, repeat the receive to infinity"); // unused but kept for compatibility
  // clang-format on
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  // print the help message
  if (vm.count("help")) {
    std::cout << boost::format("UHD RX Samples %s") % desc << std::endl;
    return ~0;
  }

  // create a usrp device
  spdlog::info("Creating the usrp device with: {}", args);
  uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
  auto time_now = usrp->get_time_now().get_real_secs();
  spdlog::info("Current time: {}", time_now);


  // detect which channels to use
  std::vector<std::string> channel_strings;
  std::vector<size_t> channel_nums;
  boost::split(channel_strings, channels, boost::is_any_of("\"',"));
  for (const auto &kChannelString : channel_strings) {
    size_t chan = boost::lexical_cast<int>(kChannelString);
    if (chan >= usrp->get_rx_num_channels()) {
      throw std::runtime_error("Invalid channel(s) specified.");
    } else {
      channel_nums.push_back(boost::lexical_cast<int>(kChannelString));
    }
  }

  // lock mboard clocks
  spdlog::info("Locking mboard clocks");
  auto clock_list = usrp->get_clock_sources(0);
  usrp->set_clock_source(ref);
  if (ref == "gpsdo") {
    while (!(usrp->get_mboard_sensor("gps_locked", 0).to_bool())) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    spdlog::info("GPSDO Locked");
    usrp->set_time_source("gpsdo");
  }
  usrp->set_time_source(ref);

  // set the sample rate
  if (not vm.count("rate")) {
    std::cerr << "Please specify a sample rate with --rate" << std::endl;
    return ~0;
  }
  spdlog::info("Setting Rx Rate: {} Msps", rate / 1e6);
  usrp->set_rx_rate(rate);
  spdlog::info("Actual RX Rate: {} MHz", usrp->get_tx_rate() / 1e6);
  spdlog::info("Setting Tx rate: {} Msps", rate / 1e6);
  usrp->set_tx_rate(rate);
  spdlog::info("Actual TX Rate: {} Msps...", usrp->get_tx_rate() / 1e6);

  if (not vm.count("freq")) {
    std::cerr << "Please specify a center frequency with --freq" << std::endl;
    return ~0;
  }
  for (size_t chan : channel_nums) {
    spdlog::info("Configuring RX Channel {}", chan);

    // set the center frequency
    spdlog::info("Setting RX Freq: {} MHz", freq / 1e6);
    uhd::tune_request_t tune_request(freq, lo_off);
    usrp->set_rx_freq(tune_request, chan);
    spdlog::info("Actual RX Freq: {} MHz", usrp->get_rx_freq(chan) / 1e6);
    spdlog::info("Setting TX Freq: {} MHz", freq / 1e6);
    usrp->set_tx_freq(tune_request, chan);
    spdlog::info("Actual TX Freq: {} MHz...", usrp->get_tx_freq() / 1e6);

    // set the rx rf gain
    if (vm.count("rx-gain")) {
      spdlog::info("Setting RX Gain: {} dB", rx_gain);
      usrp->set_rx_gain(rx_gain, chan);
      spdlog::info("Actual RX Gain: {} dB", usrp->get_rx_gain(chan));
    }
    // set the tx rf gain
    if (vm.count("tx-gain")) {
      spdlog::info("Setting TX Gain: {} dB", tx_gain);
      usrp->set_tx_gain(tx_gain, chan);
      spdlog::info("Actual TX Gain: {} dB...", usrp->get_tx_gain());
    }

    // set the analog frontend filter bandwidth
    if (vm.count("bw")) {
      spdlog::info("Setting RX Bandwidth: {} MHz", bw / 1e6);
      usrp->set_rx_bandwidth(bw, chan);
      spdlog::info("Actual RX Bandwidth: {} MHz", usrp->get_rx_bandwidth(chan) / 1e6);

      spdlog::info("Setting TX Bandwidth: {} MHz", bw / 1e6);
      usrp->set_tx_bandwidth(bw, chan);
      spdlog::info("Actual TX Bandwidth: {} MHz...", usrp->get_tx_bandwidth() / 1e6);
    }

    // set the analog frontend filter bandwidth
    if (vm.count("rx-ant")) {
      spdlog::info("Setting RX Antenna: {}", antenna);
      usrp->set_rx_antenna(antenna, chan);
      spdlog::info("Actual RX Antenna: {}", usrp->get_rx_antenna(chan));
    }
    if (vm.count("tx-ant")) {
      spdlog::info("Setting TX Antenna: {}", tx_ant);
      usrp->set_tx_antenna(tx_ant, chan);
      spdlog::info("Actual TX Antenna: {}", usrp->get_tx_antenna());
    }
  }

  usrp->set_rx_dc_offset(true);

  // create a receive streamer
  spdlog::info("Creating RX streamer...");
  uhd::stream_args_t stream_args("fc32", otw);
  stream_args.channels = channel_nums;
  uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

  // Check Ref and LO Lock detect
  // wait for LO lock
  spdlog::info("Waiting for LO lock...");
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::vector<std::string> sensor_names;
  sensor_names = usrp->get_rx_sensor_names(0);
  if (std::find(sensor_names.begin(), sensor_names.end(), "lo_locked") != sensor_names.end()) {
    uhd::sensor_value_t lo_locked = usrp->get_rx_sensor("lo_locked", 0);
    spdlog::info("Checking RX: {} ...", lo_locked.to_pp_string());
    UHD_ASSERT_THROW(lo_locked.to_bool())
  }
  sensor_names = usrp->get_mboard_sensor_names(0);
  if ((ref == "external")
      and (std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end())) {
    uhd::sensor_value_t ref_locked = usrp->get_mboard_sensor("ref_locked", 0);
    spdlog::info("Checking RX: {} ...", ref_locked.to_pp_string());
    UHD_ASSERT_THROW(ref_locked.to_bool())
  }

  //create a Tx streamer
  uhd::tx_streamer::sptr tx_stream = usrp->get_tx_stream(stream_args);
  // check Ref and LO Lock detect
  // wait for LO lock
  spdlog::info("Waiting for TX lock (1 second)");
  std::this_thread::sleep_for(std::chrono::seconds(1)); //allow for some setup time
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

  // load send buffer from file
  spdlog::info("Reading in file: {}", file);
  std::ifstream infile(file.c_str(), std::ifstream::binary);
  if (!infile.good()) {
    spdlog::error("Could not open file: {}", file);
    return ~0;
  }
  infile.seekg(0, std::ifstream::end);
  size_t tx_file_num_samps = infile.tellg() / sizeof(std::complex<float>);
  infile.seekg(0, std::ifstream::beg);
  std::vector<std::complex<float>> tx_buff(tx_file_num_samps);
  infile.read((char *) &tx_buff.front(), static_cast<std::streamsize>(tx_file_num_samps * sizeof(std::complex<float>)));
  infile.close();
  spdlog::info("tx_file_num_samps: {}", tx_file_num_samps);
  auto max_num_samps = tx_stream->get_max_num_samps();
  spdlog::info("Tx max_num_samps: {}", max_num_samps);
  if (num_samps < max_num_samps) {
    max_num_samps = num_samps;
  }


  //detect PPS edge
  spdlog::info("Setting device timestamp to 0 at next PPS");
  usrp->set_time_next_pps(uhd::time_spec_t(0.0));
  spdlog::info("Waiting for first PPS...");
  std::this_thread::sleep_for(std::chrono::seconds(1));
  spdlog::info("PPS detected, starting streaming...");

  // setup udp socket
  spdlog::info("Setting up UDP socket...");
  uhd::transport::udp_simple::sptr udp_sock =
      uhd::transport::udp_simple::make_connected(addr, udp_port);
  spdlog::info("UDP Connected");



  // setup boost asio
  boost::asio::io_context io_context;

  // register ctrl+c sigint handler
  boost::asio::signal_set signals(io_context, SIGINT);
  signals.async_wait([&](const boost::system::error_code &error, int signal_number) {
    SigIntHandler(error, signal_number, &io_context);
  });
  spdlog::info("Press Ctrl + C to stop streaming...");

  std::thread socket_thread([&]() {
    SocketWorker(io_context, std::stoi(tcp_port), usrp, rx_stream, tx_stream, tx_buff, udp_sock, tx_file_num_samps,
                 max_num_samps, num_delay, rx_file, rx_ports, tx_ports, rate, num_samps);
  });

  io_context.run();
  socket_thread.join();

  // finished
  spdlog::info("Done!");
  return EXIT_SUCCESS;
}

#pragma clang diagnostic pop