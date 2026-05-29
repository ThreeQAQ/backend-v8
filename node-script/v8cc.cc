#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <algorithm>
#include <map>
#include <memory>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "libplatform/libplatform.h"
#include "v8.h"

bool endsWith(const std::string &fullString, const std::string &ending) {
    if (fullString.length() >= ending.length()) {
        return (0 == fullString.compare (fullString.length() - ending.length(), ending.length(), ending));
    } else {
        return false;
    }
}

template <class T>
v8::MaybeLocal<T> Compile(v8::Local<v8::Context> context, v8::ScriptCompiler::Source* source,
                      v8::ScriptCompiler::CompileOptions options) {}
template <>
v8::MaybeLocal<v8::Script> Compile(v8::Local<v8::Context> context,
                           v8::ScriptCompiler::Source* source,
                           v8::ScriptCompiler::CompileOptions options) {
  return v8::ScriptCompiler::Compile(context, source, options);
}

template <>
v8::MaybeLocal<v8::Module> Compile(v8::Local<v8::Context> context,
                           v8::ScriptCompiler::Source* source,
                           v8::ScriptCompiler::CompileOptions options) {
  return v8::ScriptCompiler::CompileModule(context->GetIsolate(), source, options);
}

template <class T>
v8::MaybeLocal<T> CompileString(v8::Local<v8::Context> context,
                                   v8::Local<v8::String> source,
                                   const v8::ScriptOrigin& origin) {
  v8::ScriptCompiler::Source script_source(source, origin);
  v8::MaybeLocal<T> result = Compile<T>(context, &script_source, v8::ScriptCompiler::kNoCompileOptions);
  return result;
}

struct CodeCacheHeader {
    uint32_t MagicNumber;
    uint32_t VersionHash;
    uint32_t SourceHash;
    uint32_t FlagHash;
    uint32_t PayloadLength;
    uint32_t Checksum;
};

struct CompileOptions {
    bool force_module = false;
    bool no_cjs_wrap = false;
    bool verbose = false;
    std::string flags = "--no-lazy --no-flush-bytecode --no-enable-lazy-source-positions";
    std::string url;
    int ln = 0;
    int col = 0;
};

static std::vector<std::string> ReadFileList(const std::string& file_list_path) {
    std::vector<std::string> files;
    std::ifstream file(file_list_path);
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) {
            ++begin;
        }
        if (begin < line.size()) {
            files.push_back(line.substr(begin));
        }
    }
    return files;
}

static void ReadInputDirectory(const std::string& input_dir, std::vector<std::string>& files) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(input_dir, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& entry = *it;
        if (entry.is_regular_file(ec)) {
            const fs::path path = entry.path();
            const std::string ext = path.extension().string();
            if (ext == ".js" || ext == ".mjs") {
                files.push_back(path.string());
            }
        }
        it.increment(ec);
    }
    if (ec) {
        std::cerr << "Error scanning input dir: " << input_dir << ", " << ec.message() << std::endl;
    }
}

class CompileWorker {
public:
    CompileWorker()
        : allocator_(v8::ArrayBuffer::Allocator::NewDefaultAllocator()) {
        v8::Isolate::CreateParams create_params;
        create_params.array_buffer_allocator = allocator_.get();
        isolate_ = v8::Isolate::New(create_params);
    }

    ~CompileWorker() {
        context_.Reset();
        if (isolate_) {
            isolate_->Dispose();
        }
    }

    int CompileSingleFile(const std::string& filename, const CompileOptions& options) {
        bool is_module = options.force_module || endsWith(filename, ".mjs");
        std::string url = options.url.empty() ? filename : options.url;

        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Error opening file: " << filename << std::endl;
            return 1;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        std::string fileContent = buffer.str();
        file.close();

        v8::ScriptCompiler::CachedData* cached_data = nullptr;
        int source_length = 0;
        {
            v8::Isolate::Scope isolate_scope(isolate_);
            v8::HandleScope handle_scope(isolate_);
            v8::Local<v8::Context> context = GetContext();
            v8::Context::Scope context_scope(context);
            v8::TryCatch try_catch(isolate_);
            auto script_url = v8::String::NewFromUtf8(isolate_, url.c_str()).ToLocalChecked();
        if (!is_module && !options.no_cjs_wrap) {
            fileContent = "(function (exports, require, module, __filename, __dirname) { " + fileContent + "\n});";
        }
        v8::Local<v8::String> source =
            v8::String::NewFromUtf8(isolate_, fileContent.c_str()).ToLocalChecked();
        source_length = source->Length();
        if (is_module) {
#if V8_MAJOR_VERSION >= 12
            v8::ScriptOrigin origin(script_url, options.ln, options.col, true, -1, v8::Local<v8::Value>(), false, false, true);
#elif V8_MAJOR_VERSION > 8
            v8::ScriptOrigin origin(isolate_, script_url, options.ln, options.col, true, -1, v8::Local<v8::Value>(), false, false, true);
#else
            v8::ScriptOrigin origin(script_url, v8::Integer::New(isolate_, options.ln), v8::Integer::New(isolate_, options.col), v8::True(isolate_),
                v8::Local<v8::Integer>(), v8::Local<v8::Value>(), v8::False(isolate_), v8::False(isolate_), v8::True(isolate_));
#endif
            auto module = CompileString<v8::Module>(context, source, origin);

            if (!module.IsEmpty()) {
                cached_data = v8::ScriptCompiler::CreateCodeCache(module.ToLocalChecked()->GetUnboundModuleScript());
            }
        } else {
#if V8_MAJOR_VERSION >= 12
            v8::ScriptOrigin origin(script_url, options.ln, options.col);
#elif V8_MAJOR_VERSION > 8
            v8::ScriptOrigin origin(isolate_, script_url, options.ln, options.col);
#else
            v8::ScriptOrigin origin(script_url, v8::Integer::New(isolate_, options.ln), v8::Integer::New(isolate_, options.col));
#endif

            auto script = CompileString<v8::Script>(context, source, origin);
            if (!script.IsEmpty()) {
                cached_data = v8::ScriptCompiler::CreateCodeCache(script.ToLocalChecked()->GetUnboundScript());
            }
        }
        if (try_catch.HasCaught()) {
            v8::Local<v8::Value> stack_trace;
            if (try_catch.StackTrace(context).ToLocal(&stack_trace))
            {
                v8::String::Utf8Value info(isolate_, stack_trace);
                std::cout << (*info) << std::endl;
            }
            return 1;
        }
        }

        if (!cached_data) {
            std::cout << "cached_data is nullptr!!!" << std::endl;
            return 1;
        }

        auto dot_pos = filename.find_last_of('.');
        std::string output_filename = filename.substr(0, dot_pos == std::string::npos ? filename.size(): dot_pos) + (is_module ? ".mbc" : ".cbc");

        std::ofstream output_file(output_filename, std::ios::binary);
        if (!output_file.is_open()) {
            std::cerr << "Error creating file: " << output_filename << std::endl;
            delete cached_data;
            return 1;
        }

        output_file.write((const char*)cached_data->data, cached_data->length);
        output_file.close();

        if (options.verbose) {
            std::cout << "esm: " << is_module << std::endl;
            std::cout << "cjs: " << (!is_module && !options.no_cjs_wrap) << std::endl;
            std::cout << "v8 flags: " << options.flags << std::endl;

            std::cout << "input : " << filename << ", source length: " << source_length << std::endl;
            std::cout << "output: " << output_filename << ", bytecode length: " << cached_data->length << std::endl;
            std::cout << "url: " << url << std::endl;
            std::cout << "line offset: " << options.ln << std::endl;
            std::cout << "column offset: " << options.col << std::endl;

            const CodeCacheHeader *cch = (const CodeCacheHeader *)cached_data->data;
            std::cout << "MagicNumber : " << cch->MagicNumber << std::endl;
            std::cout << "VersionHash : " << cch->VersionHash << std::endl;
            std::cout << "SourceHash : " << cch->SourceHash << std::endl;
            std::cout << "FlagHash : " << cch->FlagHash << std::endl;
            std::cout << "PayloadLength : " << cch->PayloadLength << std::endl;
            std::cout << "Checksum : " << cch->Checksum << std::endl;
        }

        delete cached_data;
        return 0;
    }

private:
    v8::Local<v8::Context> GetContext() {
        if (context_.IsEmpty()) {
            context_.Reset(isolate_, v8::Context::New(isolate_));
        }
        return context_.Get(isolate_);
    }

    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_;
    v8::Isolate* isolate_ = nullptr;
    v8::Global<v8::Context> context_;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <filename>|--file-list=<path>|--input-dir=<path> [--module] [--no-cjs-wrap] [--verbose] [--url=<string>] [--ln=<number>] [--col=<number>] [--jobs=<number>] [v8_flag1] [v8_flag2] ..." << std::endl;
        return 1;
    }

    CompileOptions options;
    std::vector<std::string> filenames;
    std::string file_list_path;
    std::string input_dir;
    bool force_module = false;
    int jobs = 1;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--module") {
            force_module = true;
            continue;
        }
        if (arg == "--no-cjs-wrap") {
            options.no_cjs_wrap = true;
            continue;
        }
        if (arg == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (arg.rfind("--url=", 0) == 0) {
            options.url = arg.substr(6);
            continue;
        }
        if (arg.rfind("--ln=", 0) == 0) {
            options.ln = std::stoi(arg.substr(5));
            continue;
        }
        if (arg.rfind("--col=", 0) == 0) {
            options.col = std::stoi(arg.substr(6));
            continue;
        }
        if (arg.rfind("--file-list=", 0) == 0) {
            file_list_path = arg.substr(12);
            continue;
        }
        if (arg.rfind("--input-dir=", 0) == 0) {
            input_dir = arg.substr(12);
            continue;
        }
        if (arg.rfind("--jobs=", 0) == 0) {
            jobs = std::max(1, std::stoi(arg.substr(7)));
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            options.flags += (" " + arg);
            continue;
        }
        filenames.push_back(arg);
    }

    if (!file_list_path.empty()) {
        std::vector<std::string> listed_files = ReadFileList(file_list_path);
        filenames.insert(filenames.end(), listed_files.begin(), listed_files.end());
    }
    if (!input_dir.empty()) {
        ReadInputDirectory(input_dir, filenames);
    }

    if (filenames.empty()) {
        std::cerr << "No input files." << std::endl;
        return 1;
    }

    if (force_module && filenames.size() > 1) {
        std::cerr << "Warning: --module is ignored for batch input; use .mjs extension for module bytecode." << std::endl;
        force_module = false;
    }

    v8::V8::InitializeICUDefaultLocation(argv[0]);
    v8::V8::InitializeExternalStartupData(argv[0]);
    v8::V8::SetFlagsFromString(options.flags.c_str(), options.flags.size());

    std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();

    std::atomic<int> result{0};
    std::atomic<size_t> next_index{0};
    const size_t worker_count = std::min<size_t>(static_cast<size_t>(jobs), filenames.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        workers.emplace_back([&]() {
            CompileWorker worker;
            for (;;) {
                if (result.load(std::memory_order_relaxed) != 0) {
                    return;
                }

                const size_t file_index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (file_index >= filenames.size()) {
                    return;
                }

                CompileOptions file_options = options;
                file_options.force_module = force_module;
                if (options.url.empty()) {
                    file_options.url = filenames[file_index];
                }

                const int file_result = worker.CompileSingleFile(filenames[file_index], file_options);
                if (file_result != 0) {
                    int expected = 0;
                    result.compare_exchange_strong(expected, file_result, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    v8::V8::Dispose();
#if V8_MAJOR_VERSION > 9
    v8::V8::DisposePlatform();
#else
    v8::V8::ShutdownPlatform();
#endif
    return result.load(std::memory_order_relaxed);
}
