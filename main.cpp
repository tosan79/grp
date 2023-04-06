#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <vector>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <chrono>

class Params {
public:
    std::string pattern;
    std::string dir, log_file, result_file;
    int num_of_threads;

    void parse(int argc, char* argv[]);
};

struct res {
    std::vector<std::string> paths;
    std::vector<int> line_nums;
    std::vector<std::string> lines;
    int counter = 0;
};

void Params::parse(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "usage:\n" << "./grp <string pattern>\n";
        exit(0);
    }

    this->pattern = argv[1];
    this->dir = ".";
    this->log_file = "grp.log";
    this->result_file = "grp.txt";
    this->num_of_threads = 4;

    for (int i = 2; i < argc; i += 2) {
        if (std::strcmp(argv[i], "-d") == 0 || std::strcmp(argv[i], "--dir") == 0)
            this->dir = argv[i+1];
        else if (std::strcmp(argv[i], "-t")  == 0 || std::strcmp(argv[i], "--threads") == 0)
            this->num_of_threads = std::atoi(argv[i+1]);
        else if (std::strcmp(argv[i], "-l")  == 0 || std::strcmp(argv[i], "--dir") == 0)
            this->log_file = argv[i+1];
        else if (std::strcmp(argv[i], "-r")  == 0 || std::strcmp(argv[i], "--dir") == 0)
            this->result_file = argv[i+1];
        else {
            std::cout << "usage:\n" << "./grp <string pattern>\n";
            std::cout << "optional parameters:\n" << "-d (--dir) <start directory>\n";
            std::cout << "-l (--log_file) <log file>\n" << "-r (--result_file) <result file>\n";
            std::cout << "-t (--threads) <number of threads in the pool>\n";
            exit(0);
        }
    }
}

int files_with_pattern = 0;
int num_of_patterns = 0;
std::mutex fwp_mut; // protects files_with_pattern and log_map
std::mutex nop_mut; // protects num_of_patterns and res_map
std::unordered_map<std::thread::id, std::vector<std::string> > log_map; // k: thread_id -> v: file1, file2, ...
std::unordered_map<std::string, struct res> res_map; // k: filename -> v: num of patterns

bool cmp_log(const std::pair<std::thread::id, unsigned long> &p1, const std::pair<std::thread::id, unsigned long> &p2) {
    return p1.second > p2.second;
}

bool cmp_res(const std::pair<std::string, int> &p1, const std::pair<std::string, int> &p2) {
    return p1.second > p2.second;
}

void grep(std::string path, std::string &pattern) {
    std::ifstream ifs(path);
    std::string::size_type p = path.rfind("/");
    std::string filename = path.substr(p+1);
    std::string line;
    int line_num = 0;
    std::thread::id tid = std::this_thread::get_id();
    bool pat_found = false;
    while (std::getline(ifs, line)) {
        line_num++;
        if (line.find(pattern) != std::string::npos) {
            //std::cout << tid << ": " << path << ":" << line << "\n";
            {
                std::scoped_lock<std::mutex> lock(nop_mut);
                num_of_patterns++;
                
                res_map[filename].paths.push_back(path);
                res_map[filename].line_nums.push_back(line_num);
                res_map[filename].lines.push_back(line);
                res_map[filename].counter++;
            }
            if (!pat_found)
                pat_found = true;
        }
    }
    if (pat_found) {
        std::scoped_lock<std::mutex> lock(fwp_mut);
        files_with_pattern++;
        log_map[tid].push_back(filename);
    }
}

class join_threads
{
    std::vector<std::thread>& threads;
public:
    explicit join_threads(std::vector<std::thread>& threads_):
        threads(threads_)
        {}
    ~join_threads();
};

template<typename T>
class threadsafe_queue {
    mutable std::mutex mut;
    std::queue<T> data_queue;
    std::condition_variable data_cond;
public:
    threadsafe_queue()
    {}
    threadsafe_queue(threadsafe_queue const& other)
    {
        std::lock_guard<std::mutex> lk(other.mut);
        data_queue=other.data_queue;
    }
    void push(T new_value)
    {
        std::lock_guard<std::mutex> lk(mut);
        data_queue.push(new_value);
        data_cond.notify_one();
    }
    void wait_and_pop(T& value)
    {
        std::unique_lock<std::mutex> lk(mut);
        data_cond.wait(lk,[this]{return !data_queue.empty();});
        value=data_queue.front();
        data_queue.pop();
    }
    std::shared_ptr<T> wait_and_pop()
    {
        std::unique_lock<std::mutex> lk(mut);
        data_cond.wait(lk,[this]{return !data_queue.empty();});
        std::shared_ptr<T> res(std::make_shared<T>(data_queue.front()));
        data_queue.pop();
        return res;
    }
    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lk(mut);
        if(data_queue.empty())
            return false;
        value=data_queue.front();
        data_queue.pop();
        return true;
    }
    std::shared_ptr<T> try_pop()
    {
        std::lock_guard<std::mutex> lk(mut);
        if(data_queue.empty())
            return std::shared_ptr<T>();
        std::shared_ptr<T> res(std::make_shared<T>(data_queue.front()));
        data_queue.pop();
        return res;
    }
    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mut);
        return data_queue.empty();
    }
};

class ThreadPool {
    bool is_done;
    int num_of_threads;
    threadsafe_queue<std::function<void()>> works;
    std::vector<std::thread> threads;
    join_threads joiner;
    void worker();

public:
    ThreadPool(int n);
    ~ThreadPool();
    void submit_work(const std::function<void()> work);
};

join_threads::~join_threads()
{
    for(unsigned long i=0;i<threads.size();++i)
    {
        if(threads[i].joinable())
            threads[i].join();
    } 
}

ThreadPool::ThreadPool(int n)
: is_done(false), num_of_threads(n), joiner(threads) {
    try {
        for (int i = 0; i < num_of_threads; i++)
            threads.push_back(std::thread(&ThreadPool::worker, this));
        for (auto &t : threads)
            log_map[t.get_id()];
    } catch(...) {
        is_done = true;
        throw;
    }
}

void ThreadPool::worker() {
    while (!is_done) {
        std::function<void()> work;
        if (works.try_pop(work))
            work();
        else
            std::this_thread::yield();
    }
}

void ThreadPool::submit_work(const std::function<void()> work) {
    works.push(work);
}

ThreadPool::~ThreadPool() {
    is_done = true;
}

int main(int argc, char *argv[]) {
    std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();

    Params ps;
    ps.parse(argc, argv);

    std::ofstream logstream(ps.log_file);
    std::ofstream resstream(ps.result_file);

    int searched_files = 0;

    ThreadPool tp(ps.num_of_threads);

    for (auto x : std::filesystem::recursive_directory_iterator(ps.dir)) {
        if (std::filesystem::is_regular_file(x))
            searched_files++;
        tp.submit_work(std::bind(grep, x.path(), ps.pattern));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<std::pair<std::thread::id, unsigned long>> lv;
    for (auto &p : log_map)
        lv.push_back(std::make_pair(p.first, p.second.size()));
    std::sort(lv.begin(), lv.end(), cmp_log);
    
    for (auto const &x : lv) {
        logstream << x.first << ": ";
        for (auto const &it : log_map[x.first])
            logstream << it << ", ";
        logstream << "\n";
    }

    std::vector<std::pair<std::string, unsigned long>> rv;
    for (auto &p : res_map)
        rv.push_back(std::make_pair(p.first, p.second.counter));
    std::sort(rv.begin(), rv.end(), cmp_res);

    for (auto const &x : rv) {
        struct res r = res_map[x.first];
        for (unsigned long i = 0; i < r.paths.size(); i++)
            resstream << r.paths[i] << ":" << r.line_nums[i] << ": " << r.lines[i] << "\n";
    }

    std::chrono::high_resolution_clock::time_point stop = std::chrono::high_resolution_clock::now();
    std::chrono::milliseconds time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

    //ending output:
    std::cout << "Searched files: " << searched_files << "\n";
    std::cout << "Files with pattern: " << files_with_pattern << "\n";
    std::cout << "Patterns number: " << num_of_patterns << "\n"; 
    std::cout << "Result file: " << ps.result_file << "\n"; 
    std::cout << "Log file: " << ps.log_file << "\n"; 
    std::cout << "Used threads: " << ps.num_of_threads << "\n";
    std::cout << "Elapsed time: " << time_elapsed.count() << " [ms]\n";

    return 0;
}