
#include "bloaty.h"

#include <iostream>

int main(int argc, char *argv[]) {
  bloaty::RollupOutput output;
  bool ok = bloaty::BloatyMain(argc, argv, &output);
  if (ok) {
    output.Print(&std::cout);
    return 0;
  } else {
    return 1;
  }
}
