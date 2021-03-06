// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "taq.pb.h"

#include <google/bigtable/v2/bigtable.grpc.pb.h>
#include <google/protobuf/text_format.h>
#include <grpc++/grpc++.h>

#include <algorithm>
#include <chrono>
#include <ciso646>
#include <fstream>
#include <sstream>

namespace {
std::pair<std::string, std::string> parse_taq_line(int lineno,
                                                   std::string const& line);
}  // anonymous namespace

int main(int argc, char* argv[]) try {
  // ... a more interesting application would use getopt(3),
  // getopt_long(3), or Boost.Options to parse the command-line, we
  // want to keep things simple in the example ...
  if (argc != 5) {
    std::cerr
        << "Usage: create_table <project_id> <instance_id> <table> <filename>"
        << std::endl;
    return 1;
  }
  char const* project_id = argv[1];
  char const* instance_id = argv[2];
  char const* table_id = argv[3];
  char const* filename = argv[4];

  auto creds = grpc::GoogleDefaultCredentials();
  // ... notice that Bigtable has separate endpoints for different APIs,
  // we are going to upload some data, so the correct endpoint is:
  auto channel = grpc::CreateChannel("bigtable.googleapis.com", creds);

  // ... save ourselves some typing ...
  namespace bigtable = ::google::bigtable::v2;
  std::unique_ptr<bigtable::Bigtable::Stub> bt_stub(
      bigtable::Bigtable::NewStub(channel));

  std::string table_name = std::string("projects/") + project_id +
                           "/instances/" + instance_id + "/tables/" + table_id;

  // ... we just upload the first max_lines from the input file
  // because otherwise the demo can take hours to finish uploading the
  // data.  For very large uploads the application should use
  // something like Cloud Dataflow, where the upload work is sharded
  // across many clients, and should really take advantage of the
  // batch APIs ...
  int const max_lines = 1000;
  std::ifstream is(filename);
  std::string line;
  std::getline(is, line, '\n');  // ... skip the header ...
  for (int lineno = 1; lineno != max_lines and not is.eof() and is; ++lineno) {
    std::getline(is, line, '\n');
    auto kv = parse_taq_line(lineno, line);
    // ... insert a single row in each call, obviously this is not
    // very efficient, the upload_taq_batch.cc demo shows how to
    // update multiple rows at a time ...
    bigtable::MutateRowRequest request;
    request.set_table_name(table_name);
    request.set_row_key(std::move(kv.first));
    auto& set_cell = *request.add_mutations()->mutable_set_cell();
    set_cell.set_family_name("taq");
    set_cell.set_column_qualifier("message");
    set_cell.set_value(std::move(kv.second));
    // ... we use the timestamp field as a simple revision count in
    // this example, so set it to 0.  The actual timestamp of the
    // quote is stored in the key ...
    set_cell.set_timestamp_micros(0);

    bigtable::MutateRowResponse response;
    grpc::ClientContext client_context;
    auto status = bt_stub->MutateRow(&client_context, request, &response);
    if (not status.ok()) {
      std::cerr << "Error in MutateRow() request: " << status.error_message()
                << " [" << status.error_code() << "] " << status.error_details()
                << std::endl;
      // ... for this demo we just abort on the first error,
      // applications can consider a more interesting error handling
      // logic ...
      return 1;
    }
  }
  std::cout << max_lines << " quotes successfully uploaded" << std::endl;

  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception raised: " << ex.what() << std::endl;
  return 1;
} catch (...) {
  std::cerr << "Unknown exception raised" << std::endl;
  return 1;
}

namespace {
// TODO(coryan) - this should return a proto representing the TAQ data.
std::pair<std::string, std::string> parse_taq_line(
    int lineno, std::string const& line) try {
  // The data is in pipe separated fields, starting with:
  // Time: in HHMMSSNNNNNNNNN format (hours, minutes, seconds, nanoseconds).
  // Exchange: a single character
  // Symbol: a string
  // Bid_Price: float
  // Bid_Size: integer
  // Offer_Price: float
  // Offer_Size: integer
  // ... and many other fields we ignore in this demo.
  std::istringstream tokens(line);
  tokens.exceptions(std::ios::failbit);
  std::string tk;
  std::getline(tokens, tk, '|');  // fetch the timestamp

  // ... the key will be a combination of timestamp and symbol, to
  // avoid hotspotting by either ...
  std::string key = tk + "/";
  std::getline(tokens, tk, '|');  // ignore the exchange
  std::string symbol;
  std::getline(tokens, symbol, '|');  // fetch the symbol
  key += tk;

  // ... the value is built using a proto
  TAQ quote;
  std::getline(tokens, tk, '|');
  quote.set_bid_px(std::stod(tk));
  std::getline(tokens, tk, '|');
  quote.set_bid_qty(std::stol(tk));
  std::getline(tokens, tk, '|');
  quote.set_offer_px(std::stod(tk));
  std::getline(tokens, tk, '|');
  quote.set_offer_qty(std::stol(tk));

  std::string value;
  if (not quote.SerializeToString(&value)) {
    std::ostringstream os;
    os << "could not serialize quote at line #" << lineno << "(" << line << ")";
    throw std::runtime_error(os.str());
  }

  return std::make_pair(std::move(key), std::move(value));
} catch (std::exception const& ex) {
  std::ostringstream os;
  os << ex.what() << " in line #" << lineno << " (" << line << ")";
  throw std::runtime_error(os.str());
}
}  // anonymous namespace
