#include "layout_loader.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hymeta {

namespace {

// Tiny hand-rolled JSON tokenizer sufficient for our flat-array layout file.
struct Tok {
  explicit Tok(const std::string& s) : s_(s), pos_(0) {}

  void skip_ws() {
    while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) pos_++;
  }

  char peek() {
    skip_ws();
    if (pos_ >= s_.size()) return '\0';
    return s_[pos_];
  }

  char next() {
    skip_ws();
    if (pos_ >= s_.size()) throw std::runtime_error("unexpected EOF");
    return s_[pos_++];
  }

  void expect(char c) {
    char got = next();
    if (got != c) {
      std::ostringstream e;
      e << "expected '" << c << "' got '" << got << "' at pos " << pos_;
      throw std::runtime_error(e.str());
    }
  }

  std::string read_string() {
    skip_ws();
    if (s_[pos_] != '"') throw std::runtime_error("expected '\"'");
    pos_++;
    std::string out;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      out.push_back(s_[pos_++]);
    }
    pos_++;  // closing "
    return out;
  }

  int64_t read_int() {
    skip_ws();
    size_t start = pos_;
    if (s_[pos_] == '-' || s_[pos_] == '+') pos_++;
    while (pos_ < s_.size() && (std::isdigit((unsigned char)s_[pos_]) ||
                                  s_[pos_] == 'e' || s_[pos_] == 'E' ||
                                  s_[pos_] == '.' || s_[pos_] == '+' ||
                                  s_[pos_] == '-'))
      pos_++;
    std::string num = s_.substr(start, pos_ - start);
    return (int64_t)std::stoll(num);
  }

  const std::string& s_;
  size_t pos_;
};

}  // namespace

std::vector<SSTFile> load_sst_layout_json(const std::string& path,
                                           int key_size, int value_size) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open layout: " + path);
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string data = buf.str();

  int64_t kv = (int64_t)key_size + (int64_t)value_size;
  if (kv <= 0) throw std::runtime_error("key_size+value_size must be > 0");

  Tok t(data);
  t.expect('[');
  std::vector<SSTFile> out;
  int next_id = 0;

  if (t.peek() == ']') {
    t.next();
    return out;
  }

  while (true) {
    t.expect('{');
    SSTFile s;
    s.sst_id = next_id++;
    int64_t size_bytes = 0;
    while (true) {
      std::string key = t.read_string();
      t.expect(':');
      int64_t v = t.read_int();
      if (key == "level")
        s.level = (int)v;
      else if (key == "size")
        size_bytes = v;
      else if (key == "min_key")
        s.min_key = v;
      else if (key == "max_key")
        s.max_key = v;
      if (t.peek() == ',') {
        t.next();
        continue;
      }
      break;
    }
    t.expect('}');
    s.num_keys = std::max<int64_t>(1, size_bytes / kv);
    out.push_back(s);
    if (t.peek() == ',') {
      t.next();
      continue;
    }
    break;
  }
  t.expect(']');
  return out;
}

}  // namespace hymeta
