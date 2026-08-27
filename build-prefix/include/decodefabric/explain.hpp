#pragma once
#include <string>
#include <vector>
#include "decodefabric/ids.hpp"

namespace decodefabric {

// Structured Explain output. An operator can ask "why" and receive both human
// readable text and JSON carrying the same facts.
struct Explain {
  SequenceId sequence;
  RequestId request;
  std::string question;       // normalized question key, e.g. "why_waiting"
  std::string answer;         // human-readable text
  std::vector<std::string> facts;   // atomic structured facts
  std::vector<std::string> factors; // named policy/lifecycle factors

  std::string to_json() const;
};

}  // namespace decodefabric
