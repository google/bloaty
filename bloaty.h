
#ifndef BLOATY_H_
#define BLOATY_H_

#include <string>
#include <set>
#include <stdlib.h>
#include <stdint.h>

class Program;

struct File {
  File(const std::string& name_) : name(name_), source_line_weight(0) {}

  std::string name;

  // Number of object bytes attributed to this file through source line info.
  size_t source_line_weight;

  // Files we reference, ie. through a function call or variable reference.
  std::set<File*> refs;
};

struct Object {
  Object(const std::string& name_) :
      name(name_),
      vmaddr(0),
      size(0),
      weight(0),
      max_weight(0),
      file(NULL) {}

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
  uintptr_t vmaddr;
  size_t size;
  size_t weight;
  size_t max_weight;

  // Whether this is from a data section (rather than code).
  // When this is true we will scan the object for pointers.
  bool data;

  // Source file where this object was declared, if we know it.
  File* file;

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
  LineReader(FILE* file) : file_(file), eof_(false) {}
  ~LineReader() { pclose(file_); }
  LineIterator begin();
  LineIterator end();

  bool operator!=(int x) const { return !eof_; }

  void Next();

  const std::string& line() const { return line_; }
  bool eof() { return eof_; }

 private:
  FILE* file_;
  std::string line_;
  bool eof_;
};

class LineIterator {
 public:
  LineIterator(LineReader* reader) : reader_(reader) {}

  bool operator!=(const LineIterator& other) const {
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

#endif
