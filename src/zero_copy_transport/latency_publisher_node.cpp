#include "latency_publisher.hpp"
#include <rclcpp/rclcpp.hpp>

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);

  rt_middleware::PublisherConfig cfg;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--rate") cfg.rate_hz = static_cast<uint32_t>(std::stoi(argv[i+1]));
    if (arg == "--mode") {
      const std::string m(argv[i+1]);
      if (m == "loaned") cfg.mode = rt_middleware::PublishMode::Loaned;
      else if (m == "shm") cfg.mode = rt_middleware::PublishMode::Shm;
    }
  }

  rclcpp::spin(std::make_shared<rt_middleware::LatencyPublisher>(cfg));
  rclcpp::shutdown();
}
