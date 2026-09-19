// Stand-in for libFuzzer's main(): runs LLVMFuzzerTestOneInput once per file, so
// the committed corpora replay as ordinary tests with any compiler.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
  std::size_t count = 0;
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path root = argv[i];
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(root)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
      }
    } else {
      files.push_back(root);
    }
    for (const auto& file : files) {
      std::ifstream in(file, std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
      ++count;
    }
  }
  std::printf("replayed %zu inputs\n", count);
  return 0;
}
