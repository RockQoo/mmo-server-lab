#pragma once

#include <barrier>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace Bench
{
    // 여러 스레드 측정 틀: 전용 스레드 N개를 미리 만들어 두고(측정 반복 밖), 측정 한 번마다 모두 같이 출발해서
    // body(스레드 번호)를 한 번씩 돌리고 모두 끝날 때까지 기다린다. C# Bench/ThreadTeam.cs 와 같은 방법이다.
    //
    // 왜 미리 만드나(Codex 리뷰 2차 R1): 측정마다 스레드를 만들면 생성 비용과 출발 시차가 측정 시간에 섞인다. 측정
    // 한 번이 몇 ms 로 짧아 Noop 이 8코어 8스레드에서 3.8배만 늘었다. 이 틀이 스레드 수만큼 늘어나는지는 TeamNoop 으로
    // 확인한다(ObjectPoolBench.cpp). BufferPoolBench 는 아직 측정마다 스레드를 만든다(기록된 수치를 유지, PROGRESS
    // 후순위).
    //
    // body 는 스레드마다 한 번 불린다(반복은 body 안에서 돈다). 그래서 std::function 호출 비용은 측정 한 번에 한
    // 번이다.
    class ThreadTeam
    {
    public:
        explicit ThreadTeam(const int count) : _barrier(static_cast<std::ptrdiff_t>(count) + 1)
        {
            _workers.reserve(static_cast<std::size_t>(count));
            for (auto t = 0; t < count; ++t)
            {
                _workers.emplace_back([this, t] { Work(t); });
            }
        }
        ~ThreadTeam()
        {
            _stopping = true;
            _barrier.arrive_and_wait(); // 작업자가 출발 자리에서 깨어나 멈춘다. 그 뒤 _workers 소멸 = join
        }
        ThreadTeam(const ThreadTeam&) = delete;
        ThreadTeam& operator=(const ThreadTeam&) = delete;
        ThreadTeam(ThreadTeam&&) = delete;
        ThreadTeam& operator=(ThreadTeam&&) = delete;

        // 모든 작업자가 같이 출발해 body(번호)를 돌리고, 모두 끝나면 돌아온다.
        void Run(std::function<void(int)> body)
        {
            _body = std::move(body);
            _barrier.arrive_and_wait(); // 출발(barrier 가 앞의 쓰기를 작업자에게 보이게 한다)
            _barrier.arrive_and_wait(); // 끝
        }

    private:
        void Work(const int index)
        {
            while (true)
            {
                _barrier.arrive_and_wait();
                if (_stopping)
                {
                    return;
                }
                _body(index);
                _barrier.arrive_and_wait();
            }
        }

        std::barrier<> _barrier; // 작업자 N + 측정 스레드 1
        std::function<void(int)> _body;
        bool _stopping{};                   // barrier 앞에서 쓰고 뒤에서 읽는다(barrier 가 순서를 맞춘다)
        std::vector<std::jthread> _workers; // 마지막 멤버: 가장 먼저 소멸(join)한다
    };
} // namespace Bench
