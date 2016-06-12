
#ifndef BLOATY_H_
#define BLOATY_H_

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stdlib.h>
#include <stdint.h>

#include "re2/re2.h"

#define BLOATY_DISALLOW_COPY_AND_ASSIGN(class_name) \
  class_name(const class_name&) = delete; \
  void operator=(const class_name&) = delete;

class Program;

struct Rule {
  Rule(const std::string& name_) : name(name_), pretty_name(name_) {}
  std::string name;
  std::string pretty_name;
  size_t size = 0;  // Based on source line calculations.
  size_t weight = 0;
  size_t max_weight = 0;
  uint32_t id;
  std::unordered_set<Rule*> refs;
  Rule* dominator = nullptr;
};

struct File {
  File(const std::string& name_) : name(name_) {}

  std::string name;
  Rule* rule = nullptr;

  // Number of object bytes attributed to this file through source line info.
  size_t source_line_weight = 0;
  size_t object_size = 0;

  // Files we reference, ie. through a function call or variable reference.
  std::set<File*> refs;
};

struct Object {
  Object(const std::string& name_) : name(name_) {}

  // Declared name of the symbol.
  std::string name;

  // A single object can be known by multiple names.  We take the first one
  // lexicographically as the primary name and others (if any) live here as
  // aliases.
  std::set<std::string> aliases;

  // Name possibly put through c++filt, but also reduced to remove overloads if possible.
  std::string pretty_name;

  void SetSize(size_t size_) {
    size = size_;
    weight = size_;
  }

  uint32_t id;
  uintptr_t vmaddr = 0;
  size_t size = 0;
  size_t weight = 0;
  size_t max_weight = 0;

  // Whether this is from a data section (rather than code).
  // When this is true we will scan the object for pointers.
  bool data;

  // Source file where this object was declared, if we know it.
  File* file = nullptr;

  // The dominator (https://en.wikipedia.org/wiki/Dominator_(graph_theory))
  // for this node.
  //
  // NULL for the entry symbol, or for symbols not reachable from the entry
  // symbol.
  //
  // If there isn't an entry symbol we can't calculate dominators at all.
  Object* dominator = nullptr;

  // Objects we reference, ie. through a function call or variable reference.
  std::set<Object*> refs;
};

class ProgramDataSink {
 public:
  ProgramDataSink(Program* program) : program_(program) {}

  Object* AddObject(const std::string& name, uintptr_t vmaddr, size_t size,
                    bool data);
  void AddObjectAlias(Object* obj, const std::string& name);
  Object* FindObjectByName(const std::string& name);
  Object* FindObjectByAddr(uintptr_t addr);
  Object* FindObjectContainingAddr(uintptr_t addr);
  void AddRef(Object* from, Object* to);
  void SetEntryPoint(Object* obj);
  void AddFileMapping(uintptr_t vmaddr, uintptr_t fileoff, size_t filesize);
  File* GetOrCreateFile(const std::string& filename);

 private:
  Program* program_;
};


/** LineReader ****************************************************************/

// Provides range-based for, for iterating over lines in a pipe.

class LineIterator;

class LineReader {
 public:
  LineReader(FILE* file, bool pclose)
      : file_(file), eof_(false), pclose_(pclose) {}
  ~LineReader() {
    if (pclose_) {
      pclose(file_);
    } else {
      fclose(file_);
    }
  }
  LineIterator begin();
  LineIterator end();

  void Next();

  const std::string& line() const { return line_; }
  bool eof() { return eof_; }

 private:
  FILE* file_;
  std::string line_;
  bool eof_;
  bool pclose_;
};

class LineIterator {
 public:
  LineIterator(LineReader* reader) : reader_(reader) {}

  bool operator!=(const LineIterator& other) const {
    // Hack for range-based for.
    return !reader_->eof();
  }

  void operator++() { reader_->Next(); }

  const std::string& operator*() const {
    return reader_->line();
  }

 private:
  LineReader* reader_;
};

LineReader ReadLinesFromPipe(const std::string& cmd);

// Provided by arch-specific platform module.
void ReadELFObjectData(const std::string& filename, ProgramDataSink* sink);
std::string ReadELFBuildId(const std::string& filename);
void ReadMachOObjectData(const std::string& filename, ProgramDataSink* sink);
std::string ReadMachOBuildId(const std::string& filename);
void ParseELFLineInfo(
    const std::string& filename,
    const std::unordered_map<std::string, Rule*>& source_files);
void ParseELFDebugInfo(const std::string& filename, ProgramDataSink* sink);
void ParseELFSymbols(const std::string& filename,
                            ProgramDataSink* sink);
void ParseELFSections(const std::string& filename,
                             ProgramDataSink* sink);
void ParseELFFileMapping(const std::string& filename,
                             ProgramDataSink* sink);

namespace bloaty {
void GetFunctionFilePairs(const std::string& filename, ProgramDataSink* sink);
}

inline bool TryGetFallbackFilename(const std::string& name, std::string* fallback) {
  // blaze-out/gcc-4.X.Y-crosstool-v18-hybrid-grtev4-k8-opt/genfiles/translating/lib/proto/styled_word.proto.h
  if (RE2::FullMatch(name, "blaze-out.*genfiles\\/(.*).proto.h", fallback)) {
    std::cerr << "YoTrying: " << *fallback << "\n";
    *fallback += ".pb.h";
    return true;
  } else if (RE2::FullMatch(name, "blaze-out.*genfiles\\/(.*)", fallback)) {
    std::cerr << "Trying: " << *fallback << "\n";
    return true;
  } else if (name.find("third_party/crosstool/v18/stable/toolchain") == 0 ||
             name.find("/usr/crosstool") == 0 ||
             name.find("blaze-out/gcc-4.X.Y-crosstool-v18-hybrid-grtev4-k8-opt/"
                       "genfiles/"
                       "third_party/libunwind") == 0 ||
             name.find("third_party/grte") == 0) {
    *fallback = "third_party/crosstool/v18/stable/toolchain";
    return true;
  }

  return false;
}

#endif
