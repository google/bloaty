// Copyright 2016 Google Inc. All Rights Reserved.
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

#include "bloaty.h"
#include "bloaty.pb.h"

#include <iostream>

int main(int argc, char *argv[]) {
  bloaty::Options options;
  bloaty::OutputOptions output_options;
  std::string error;
  if (!bloaty::ParseOptions(false, &argc, &argv, &options, &output_options,
                            &error)) {
    if (!error.empty()) {
      fprintf(stderr, "bloaty: %s\n", error.c_str());
      return 1;
    } else {
      return 0;  // --help or similar.
    }
  }

  bloaty::RollupOutput output;
  bloaty::MmapInputFileFactory mmap_factory;
  if (!bloaty::BloatyMain(options, mmap_factory, &output, &error)) {
    if (!error.empty()) {
      fprintf(stderr, "bloaty: %s\n", error.c_str());
    }
    return 1;
  }

  if (!options.dump_raw_map()) {
    output.Print(output_options, &std::cout);
  }
  return 0;
}
