#include <dbcore/sm-alloc.h>
#include <dbcore/sm-config.h>
#include <dbcore/sm-coroutine.h>
#include <dbcore/sm-thread.h>
#include <masstree/masstree_btree.h>
#include <varstr.h>

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#include "utils/record.h"

template <typename T>
using task = ermia::dia::task<T>;

class Context {
   public:
    Context() {
        init();
        masstree_ = new ermia::ConcurrentMasstree();
    }
    ~Context() {
        delete masstree_;
        fini();
    }

    void run() {
        std::cout << "Randomly generating " << k_record_num << " records..."
                  << std::endl;
        all_records_ = genSequentialRecords(k_record_num, k_key_len);

        running_threads_ = getThreads(k_threads);
        std::cout << "Running perf use " << running_threads_.size()
                  << " threads" << std::endl;

        loadRecords(all_records_);
        verifyInserted(all_records_);

        spin_barrier setup_barrier(running_threads_.size());
        spin_barrier start_barrier(running_threads_.size() > 0 ? 1 : 0);
        std::vector<uint32_t> counter(running_threads_.size(), 0);

        searchRecords(setup_barrier, start_barrier, counter);

        ermia::volatile_write(is_running_, true);
        start_barrier.count_down();
        for (uint32_t i = 0; i < k_running_secs; i++) {
            sleep(1);
            std::cout << "Run after " << i << " seconds..." << std::endl;
        }
        ermia::volatile_write(is_running_, false);

        std::cout << "Perf completed" << std::endl;
        uint32_t counter_sum =
            std::accumulate(counter.begin(), counter.end(), 0);
        std::cout << "Total throughput: " << counter_sum << std::endl;
        std::cout << "Avg throughput(per sec): " << counter_sum / k_running_secs
                  << std::endl;

        returnThreads(running_threads_);
    }

    virtual void searchRecords(spin_barrier &setup_barrier, spin_barrier &start_barrier,
                std::vector<uint32_t> &counter) = 0;

   protected:
    static constexpr int k_record_num = 30000000;
    static constexpr int k_key_len = 8;
    static constexpr int k_threads = 10;
    static constexpr int k_batch_size = 25;
    static constexpr int k_running_secs = 10;

    bool is_running_;

    ermia::ConcurrentMasstree *masstree_;
    std::vector<Record> all_records_;
    std::vector<ermia::thread::Thread *> running_threads_;

    PROMISE(bool)
    searchByKey(const std::string &key, ermia::OID *out_value,
                ermia::epoch_num e) {
        bool res = AWAIT masstree_->search(ermia::varstr(key.data(), key.size()),
                                           *out_value, e, nullptr);

        RETURN res;
    }

   private:
    void loadRecords(const std::vector<Record> &records) {
        const uint32_t records_per_threads =
            ceil(records.size() / static_cast<float>(running_threads_.size()));
        std::cout << "Start loading " << records.size() << " records..."
                  << std::endl;
        uint32_t loading_begin_idx = 0;
        for (uint32_t i = 0; i < running_threads_.size(); i++) {
            ermia::thread::Thread::Task load_task = [&, i, loading_begin_idx](
                                                        char *) {
                uint32_t records_to_load = std::min(
                    records_per_threads,
                    static_cast<uint32_t>(records.size() - loading_begin_idx));
                printf(
                    "thread ID(%d): start loading %d records from index "
                    "%d...\n",
                    i, records_to_load, loading_begin_idx);

                ermia::dia::coro_task_private::memory_pool memory_pool;
                for (uint32_t j = loading_begin_idx;
                     j < loading_begin_idx + records_to_load; j++) {
                    const Record &record = records[j];
                    ermia::TXN::xid_context xid_ctx;
                    xid_ctx.begin_epoch = ermia::MM::epoch_enter();
                    assert(
                        sync_wait_coro(masstree_->insert(
                            ermia::varstr(record.key.data(), record.key.size()),
                            record.value, &xid_ctx, nullptr, nullptr)) &&
                        "Fail to insert record into masstree!");
                    ermia::MM::epoch_exit(0, xid_ctx.begin_epoch);
                }

                printf("thread ID(%d): finish loading %d records\n", i,
                       records_to_load);
            };
            running_threads_[i]->StartTask(load_task, nullptr);
            loading_begin_idx += records_per_threads;
        }
        for(auto & th : running_threads_) {
            th->Join();
        }
        std::cout << "Finish loading " << records.size() << " records"
                  << std::endl;
    }

    void verifyInserted(const std::vector<Record> &records) {
        ermia::dia::coro_task_private::memory_pool memory_pool;
        for (auto &record : records) {
            ermia::OID value;
            sync_wait_coro(masstree_->search(
                ermia::varstr(record.key.data(), record.key.size()), value, 0,
                nullptr));
            assert(value == record.value);
        }
    }

    static void init() {
        ermia::config::node_memory_gb = 2;
        ermia::config::num_backups = 0;
        ermia::config::physical_workers_only = true;
        ermia::config::threads = 20;

        ermia::config::init();
        ermia::MM::prepare_node_memory();
    }

    static void fini() {
        ermia::MM::free_node_memory();
        ermia::thread::Finalize();
    }

    static std::vector<ermia::thread::Thread *> getThreads(unsigned int num) {
        std::vector<ermia::thread::Thread *> idle_threads;
        for (uint32_t i = 0; i < std::min(num, ermia::config::threads); i++) {
            idle_threads.emplace_back(ermia::thread::GetThread(true));
            assert(idle_threads.back() && "Threads not available!");
        }
        return idle_threads;
    }

    static void returnThreads(
        std::vector<ermia::thread::Thread *> &running_threads_) {
        for (ermia::thread::Thread *th : running_threads_) {
            th->Join();
            ermia::thread::PutThread(th);
        }
        running_threads_.clear();
    }
};

#ifdef USE_STATIC_COROUTINE

class ContextNestedCoro : public Context {
   public:
    void searchRecords(spin_barrier &setup_barrier, spin_barrier &start_barrier,
                std::vector<uint32_t> &counter) {
        for (uint32_t i = 0; i < running_threads_.size(); i++) {
            ermia::thread::Thread::Task search_task = [&, i](char *) {
                auto seed =
                    std::chrono::system_clock::now().time_since_epoch().count();
                std::default_random_engine generator(seed);
                std::uniform_int_distribution<int> distribution(
                    0, all_records_.size() - 1);
                std::array<task<bool>, k_batch_size> task_queue;
                std::array<const Record *, k_batch_size> task_records = {nullptr};
                std::array<ermia::OID, k_batch_size> task_rets = {0};
                std::array<ermia::dia::coro_task_private::coro_stack,
                           k_batch_size>
                    coro_stacks;
                ermia::dia::coro_task_private::memory_pool memory_pool;
                setup_barrier.count_down();

                start_barrier.wait_for();
                while (ermia::volatile_read(is_running_)) {
                    for (uint32_t j = 0; j < k_batch_size; j++) {
                        task<bool> &t = task_queue[j];
                        if (t.valid()) {
                            if (t.done()) {
                                ASSERT(t.get_return_value());
                                ASSERT(task_rets[j] == task_records[j]->value);
                                counter[i]++;
                                t = task<bool>(nullptr);
                            } else {
                                t.resume();
                            }
                        }

                        if (!t.valid()) {
                            const Record &record = all_records_[distribution(generator)];
                            task_records[j] = &record;
                            t = searchByKey(record.key, &task_rets[j], 0);
                            t.set_call_stack(&(coro_stacks[j]));
                        }
                    }
                }

                for (auto &t : task_queue) {
                    t.destroy();
                }
            };
            running_threads_[i]->StartTask(search_task, nullptr);
        }
    }
};

#else

class ContextSequential : public Context {
   public:
    void searchRecords(spin_barrier &setup_barrier, spin_barrier &start_barrier,
                std::vector<uint32_t> &counter) {
        for (uint32_t i = 0; i < running_threads_.size(); i++) {
            ermia::thread::Thread::Task search_task = [&, i](char *) {
                auto seed =
                    std::chrono::system_clock::now().time_since_epoch().count();
                std::default_random_engine generator(seed);
                std::uniform_int_distribution<int> distribution(
                    0, all_records_.size() - 1);
                setup_barrier.count_down();

                start_barrier.wait_for();
                while (ermia::volatile_read(is_running_)) {
                    const Record & record = all_records_[distribution(generator)];
                    ermia::OID value_out;
                    bool res = sync_wait_coro(
                        searchByKey(record.key, &value_out, 0));
                    ASSERT(res);
                    ASSERT(value_out = record.value);
                    counter[i]++;
                }
            };
            running_threads_[i]->StartTask(search_task, nullptr);
        }
    }
};

class ContextAmac : public Context {
   public:
    void search(spin_barrier &setup_barrier, spin_barrier &start_barrier,
                std::vector<uint32_t> &counter) {
        for (uint32_t i = 0; i < running_threads_.size(); i++) {
            ermia::thread::Thread::Task search_task = [&, i](char *) {
                auto seed =
                    std::chrono::system_clock::now().time_since_epoch().count();
                std::default_random_engine generator(seed);
                std::uniform_int_distribution<int> distribution(
                    0, all_records_.size() - 1);
                std::vector<ermia::ConcurrentMasstree::AMACState> amac_states;
                amac_states.reserve(k_batch_size);
                std::array<ermia::varstr, k_batch_size> amac_params;
                setup_barrier.count_down();

                start_barrier.wait_for();
                while (ermia::volatile_read(is_running_)) {
                    for(uint32_t i = 0; i < k_batch_size; i++) {
                        const Record & record = all_records_[distribution(generator)];
                        amac_params[i] = ermia::varstr(record.key.data(), record.key.size());
                        amac_states.emplace_back(&(amac_params[i]));
                    }

                    if(amac_states.empty()) {
                        break;
                    }

                    masstree_->search_amac(amac_states, 0);
                    amac_states.clear();

                    counter[i] += k_batch_size;
                }
            };
            running_threads_[i]->StartTask(search_task, nullptr);
        }
    }
};

#endif

int main() {
#ifdef USE_STATIC_COROUTINE
    ContextNestedCoro context;
#else
    ContextSequential context;
#endif
    context.run();
    return 0;
}
