#include "latency_subscriber.hpp"
#include "csv_logger.hpp"
#include <rclcpp/rclcpp.hpp>

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);

  rt_middleware::SubscriberConfig cfg;
  std::string output{"results/latency.csv"};

  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--rate")   cfg.publish_rate_hz = static_cast<uint32_t>(std::stoi(argv[i+1]));
    if (arg == "--output") output = argv[i+1];
    if (arg == "--mode") {
      const std::string m(argv[i+1]);
      if (m == "loaned") cfg.transport_mode = rt_middleware::TransportMode::Loaned;
      else if (m == "shm") cfg.transport_mode = rt_middleware::TransportMode::Shm;
    }
  }
  cfg.csv_output_path = output;

  auto logger = std::make_shared<rt_middleware::CsvLogger>(output);
  rclcpp::spin(std::make_shared<rt_middleware::LatencySubscriber>(cfg, logger));
  rclcpp::shutdown();
}
