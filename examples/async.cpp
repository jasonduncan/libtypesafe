// Non-blocking use: start several calls, keep working, and call pump() from your own loop.
// Needs TYPESAFE_API_KEY.
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

int main() {
  auto client = ts::Client::create();
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }

  const std::vector<std::string> messages = {
      "I was charged twice this month.",
      "The export button does nothing on Safari.",
      "Do you have a student discount?",
  };

  struct Job {
    std::string message;
    ts::Key<ts::NoulAnswer> urgent;
    ts::Key<ts::ChoiceAnswer> topic;
    ts::Pending<ts::SystemOneResult> pending;
  };
  std::vector<Job> jobs;
  for (const auto& message : messages) {
    ts::Questions q;
    Job job;
    job.message = message;
    job.urgent = q.add("urgent", ts::noul("The customer needs a reply today."));
    job.topic = q.add("topic", ts::choice("What is this about?", "billing", "technical", "other"));
    job.pending = client->system_one_async(message, std::move(q));
    jobs.push_back(std::move(job));
  }

  // The loop keeps doing its own work; pump() never blocks.
  std::size_t remaining = jobs.size();
  while (remaining > 0) {
    client->pump();
    for (auto& job : jobs) {
      if (!job.pending.done()) continue;
      auto result = job.pending.take();  // empties the handle, so each job is reported once
      --remaining;
      if (!result) {
        std::cout << job.message << " -> " << result.error().message << '\n';
        continue;
      }
      std::cout << job.message << " -> " << (*result)[job.topic].choice
                << ((*result)[job.urgent].yes() ? " (urgent)" : "") << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // stand-in for other work
  }
}
