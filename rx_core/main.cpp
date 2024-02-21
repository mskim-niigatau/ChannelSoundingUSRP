#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedMacroInspection"

#include <uhd/types/tune_request.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#define _WIN32_WINNT 0x0601 // NOLINT(bugprone-reserved-identifier)
#include <uhd/transport/udp_simple.hpp>
#include <boost/format.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <complex>
#include <fstream>
#include <csignal>
#include <spdlog/spdlog.h>
#if defined(_WIN32)
#include <winsock2.h>
#endif

// GPIO pin config
#define AMP_GPIO_MASK 0x00
#define MAN_GPIO_MASK 0xFF
#define ATR_MASKS (AMP_GPIO_MASK | MAN_GPIO_MASK)
#define ATR_CONTROL (AMP_GPIO_MASK)
#define GPIO_DDR (AMP_GPIO_MASK | MAN_GPIO_MASK)

namespace po = boost::program_options;

static bool stop_signal_called = false;
#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCDFAInspection"
void SigIntHandler(int) {
  stop_signal_called = true;
}
#pragma clang diagnostic pop

void GpioWorker(const uhd::usrp::multi_usrp::sptr &usrp,
                double rate,
                size_t num_samps,
                size_t tx_ports,
                size_t rx_ports,
                double base_time) {
  // basic ATR configuation
  usrp->set_gpio_attr("FP0", "CTRL", ATR_CONTROL, ATR_MASKS);
  usrp->set_gpio_attr("FP0", "DDR", GPIO_DDR, ATR_MASKS);

  //start GPIO control
  unsigned int gpio_state;
  auto command_time = base_time;
  for (int i = 0; i < tx_ports; i++) {
    for (int j = 0; j < rx_ports; j++) {
      gpio_state = MAN_GPIO_MASK & ~(1 << j);
      usrp->set_command_time(command_time);
      usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
      usrp->clear_command_time();
      command_time += (static_cast<double>(num_samps) * 2 / rate);
    }
  }
  gpio_state = 0xFF;
  command_time += (static_cast<double>(num_samps) * 2 / rate);
  usrp->set_command_time(command_time);
  usrp->set_gpio_attr("FP0", "OUT", gpio_state, ATR_MASKS);
  usrp->clear_command_time();
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
int UHD_SAFE_MAIN(int argc, char *argv[]) {
#pragma clang diagnostic pop
  // variables to be set by po
  std::string args, subdev, ref, otw, channels, antenna, file_path, addr, udp_port, tcp_port;
  size_t num_samps, rx_ports, tx_ports, num_delay;
  double rate, freq, gain, bw, lo_off;
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
      ("rate", po::value<double>(&rate), "rate of incoming samples")
      ("lo_off", po::value<double>(&lo_off)->default_value(-1),
       "offset from the center frequency")
      ("freq", po::value<double>(&freq), "RF center frequency in Hz")
      ("gain", po::value<double>(&gain), "gain for the RF chain")
      ("bw", po::value<double>(&bw), "analog frontend filter bandwidth in Hz")
      ("subdev", po::value<std::string>(&subdev), "subdevice specification")
      ("ref", po::value<std::string>(&ref)->default_value("internal"),
       "reference source (internal, external, mimo, gpsdo)")
      ("otw", po::value<std::string>(&otw)->default_value("sc16"),
       "specify the over-the-wire sample mode")
      ("channels", po::value<std::string>(&channels)->default_value("0"),
       "which channels to use")
      ("antenna", po::value<std::string>(&antenna)->default_value("RX2"),
       "which antenna to use (TX/RX, RX2, CAL)")
      ("samps",
       po::value<size_t>(&num_samps)->default_value(256),
       "total number of samples to receive")
      ("rx-ports", po::value<size_t>(&rx_ports)->default_value(8), "number of Rx ports")
      ("tx-ports", po::value<size_t>(&tx_ports)->default_value(8), "number of Tx ports")
      ("file", po::value<std::string>(&file_path)->default_value(""), "file path to write to")
      ("delay", po::value<size_t>(&num_delay)->default_value(0), "delay samples")
      ("addr", po::value<std::string>(&addr)->default_value("127.0.0.1"), "IP address")
      ("port", po::value<std::string>(&udp_port)->default_value("12345"), "port number")
      ("tcp-port", po::value<std::string>(&tcp_port)->default_value(""), "TCP port number")
      ("repeat", "if set, repeat the receive to infinity");
  // clang-format on
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  // print the help message
  if (vm.count("help")) {
    std::cout << boost::format("UHD RX Samples %s") % desc << std::endl;
    return ~0;
  }

  // if defined TCP port, use TCP
  if (!tcp_port.empty()) {
    use_tcp = true;
  }
#if defined(_WIN32)
  WSADATA wsa_data;
  if (use_tcp) {
    if (WSAStartup(MAKEWORD(2, 0), &wsa_data) != 0) {
      spdlog::error("WSAStartup failed");
      return 1;
    }
  }
  struct sockaddr_in tcp_addr{};
  SOCKET tcp_socket;
  if (use_tcp) {
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(static_cast<uint16_t>(std::stoi(tcp_port)));
    tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(tcp_socket, (struct sockaddr *) &tcp_addr, sizeof(tcp_addr)) < 0) {
      spdlog::error("bind failed");
      return 1;
    }

    listen(tcp_socket, 1);
  }

  // socket setup for TCP client
  SOCKET tcp_client_socket;
  struct sockaddr_in tcp_client_addr{};
  int tcp_client_addr_len = sizeof(tcp_client_addr);

  if (use_tcp) {
    // send a single packet
    spdlog::info("Waiting for TCP connection on port {}", tcp_port);
    tcp_client_socket = accept(tcp_socket, (struct sockaddr *) &tcp_client_addr, &tcp_client_addr_len);
    spdlog::info("TCP connection established");
  }
#elif defined(__linux__)
  // TODO: LINUX用実装
#endif


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
  usrp->set_time_source(ref);

  // set the sample rate
  if (not vm.count("rate")) {
    std::cerr << "Please specify a sample rate with --rate" << std::endl;
    return ~0;
  }
  spdlog::info("Setting Rx Rate: {} Msps", rate / 1e6);
  usrp->set_rx_rate(rate);
  spdlog::info("Actual RX Rate: {} MHz", usrp->get_tx_rate() / 1e6);

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

    // set the rf gain
    if (vm.count("gain")) {
      spdlog::info("Setting RX Gain: {} dB", gain);
      usrp->set_rx_gain(gain, chan);
      spdlog::info("Actual RX Gain: {} dB", usrp->get_rx_gain(chan));
    }

    // set the analog frontend filter bandwidth
    if (vm.count("bw")) {
      spdlog::info("Setting RX Bandwidth: {} MHz", bw / 1e6);
      usrp->set_rx_bandwidth(bw, chan);
      spdlog::info("Actual RX Bandwidth: {} MHz", usrp->get_rx_bandwidth(chan) / 1e6);
    }

    // set the analog frontend filter bandwidth
    if (vm.count("antenna")) {
      spdlog::info("Setting RX Antenna: {}", antenna);
      usrp->set_rx_antenna(antenna, chan);
      spdlog::info("Actual RX Antenna: {}", usrp->get_rx_antenna(chan));
    }
  }

  usrp->set_rx_dc_offset(true);

  // create a receive streamer
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

  // register ctrl+c sigint handler
  std::signal(SIGINT, &SigIntHandler);
  spdlog::info("Press Ctrl + C to stop streaming...");

  spdlog::info("Setting device timestamp to 0 at next PPS");
  usrp->set_time_next_pps(uhd::time_spec_t(0.0));
  spdlog::info("Waiting for first PPS...");
  std::this_thread::sleep_for(std::chrono::seconds(1));
  spdlog::info("PPS detected, starting streaming...");

  bool status = true;
  // start streaming
  while (true) {
    // buffer for tcp data
    char tcp_send_buffer[256], tcp_recv_buffer[256];
    int tcp_bytes_received;
    bool recv_flag = false;
    if (use_tcp) {
#if defined(_WIN32)
      // Notify client that ready to receive
      if (status) tcp_send_buffer[0] = '1';
      else tcp_send_buffer[0] = '0';
      tcp_send_buffer[1] = '\0';
      spdlog::info("Ready to receive...");
      if (send(tcp_client_socket, tcp_send_buffer, static_cast<int>(strlen(tcp_send_buffer)), 0) < 0) {
        spdlog::error("send failed");
        break;
      }
      spdlog::info("Waiting for client to start streaming...");
      while (!recv_flag) {
        tcp_bytes_received = recv(tcp_client_socket, tcp_recv_buffer, sizeof(tcp_recv_buffer), 0);
        if (tcp_bytes_received < 0) {
          spdlog::error("recv failed");
          break;
        } else if (tcp_bytes_received == 0) {
          spdlog::error("recv failed, connection closed");
          break;
        } else {
          recv_flag = true;
          break;
        }
      }
#elif defined(__linux)
      // TODO: LINUX用実装
#endif
    } else {
      recv_flag = true;
    }

    if (!recv_flag) {
      break;
    }

    spdlog::info("Starting streaming...");
    // setup streaming
    auto total_num_samps = num_samps * tx_ports * rx_ports * 2 + num_delay;
    time_now = usrp->get_time_now().get_real_secs();
    auto recv_time = std::ceil(time_now * 5) / 5;
    if (recv_time < time_now + 0.05) {
      recv_time += 0.2;
    }
    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
    stream_cmd.num_samps = total_num_samps;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = uhd::time_spec_t(recv_time);
    rx_stream->issue_stream_cmd(stream_cmd);
    spdlog::info("Begin streaming {} samples at {}", total_num_samps, recv_time);

    // start gpio thread
    std::thread gpio_thread([&]() {
      GpioWorker(usrp, rate, num_samps, tx_ports, rx_ports, recv_time + static_cast<double>(num_delay) / rate);
    });

    // meta-data will be filled in by recv()
    uhd::rx_metadata_t md;

    // allocate buffer to receive with samples
    std::vector<std::complex<float>> buff(rx_stream->get_max_num_samps());
    std::vector<std::complex<float>> buffs;

//    // setup udp socket
    uhd::transport::udp_simple::sptr udp_sock =
        uhd::transport::udp_simple::make_connected(addr, udp_port);

    // the first call to recv() will block this many seconds before receiving
    double timeout = 0.5;

    size_t num_acc_samps = 0;
    while (num_acc_samps < total_num_samps) {

      // receive a single packet
      size_t num_rx_samps;
      try {
        num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md, timeout);
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
      buffs.insert(buffs.end(), buff.begin(), buff.end());
    }

    if (num_acc_samps < total_num_samps) {
      spdlog::warn("Did not receive all samples: {} out of {}", num_acc_samps, total_num_samps);
      buff.clear();
      buffs.clear();
      status = false;
    } else {
      buffs.erase(buffs.begin(), buffs.begin() + static_cast<int>(num_delay));
      num_acc_samps -= num_delay;
      auto rcvd_time = usrp->get_time_now().get_real_secs();
      spdlog::info("Recieved {} samples at {}", num_acc_samps, rcvd_time);
      size_t type_size = sizeof(buffs.front());
      while (num_acc_samps > 0) {
        size_t num_tx_samps;
        if (num_acc_samps < 2000) {
          udp_sock->send(boost::asio::buffer(buffs, num_acc_samps * type_size));
          num_tx_samps = num_acc_samps;
        } else {
          udp_sock->send(boost::asio::buffer(buffs, 2000 * type_size));
          num_tx_samps = 2000;
        }
        buffs.erase(buffs.begin(), buffs.begin() + static_cast<int>(num_tx_samps));
        num_acc_samps -= num_tx_samps;
        std::this_thread::sleep_for(std::chrono::microseconds(1));
      }
      buffs.clear();
      status = true;
    }

    if (vm.count("file")) {
      std::ofstream outfile(file_path, std::ofstream::binary);
      outfile.write((const char *) &buffs.front(), std::streamsize(num_acc_samps * sizeof(std::complex<float>)));
      outfile.close();
    }

    gpio_thread.join();
    if (stop_signal_called or !vm.count("repeat")) {
      break;
    }
  }

  // finished
  if (use_tcp) {
#if defined(_WIN32)
    closesocket(tcp_socket);
    WSACleanup();
#elif defined(__linux)
    // TODO: LINUX用実装
#endif
  }
  spdlog::info("Done!");
  return EXIT_SUCCESS;
}

#pragma clang diagnostic pop