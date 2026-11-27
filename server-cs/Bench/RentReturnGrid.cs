using System.Runtime.InteropServices;

namespace Bench;

// 빌리는 스레드 N개 → 반납하는 스레드 M개 측정 틀(풀 벤치마크 공용, C++ Bench/RentReturnGrid.h 와 같은 방법).
//
// 실무 모양: 여러 스레드가 빌리고 다른 스레드가 반납한다(4:1, 1:4 등). 비율이 기울면 한쪽 스레드 캐시는 늘 넘치고 다른
// 쪽은 늘 빈다. 넘기는 방법은 (빌리는 쪽, 반납하는 쪽) 쌍마다 우편함 하나: 빌리는 쪽은 BlockSize 개를 빌려 블록을 채우고
// 반납하는 쪽을 차례로 바꿔 가며 넣는다. 반납하는 쪽은 자기 열의 우편함 N개를 돌며 블록을 받아 모두 반납한다.
// 1:1 은 기준선(건너가기만, 다툼 거의 없음)이다.
internal sealed class RentReturnGrid<T> : IDisposable where T : class
{
    private const int BlockSize = 64;

    private readonly int _renters;
    private readonly int _returners;
    private readonly RentReturnCell[] _cells;
    private readonly ThreadTeam _team;

    public RentReturnGrid(int renters, int returners)
    {
        _renters = renters;
        _returners = returners;
        _cells = new RentReturnCell[renters * returners];
        for (var i = 0; i < _cells.Length; ++i)
        {
            _cells[i] = new RentReturnCell(new T?[BlockSize], new T?[BlockSize]);
        }
        _team = new ThreadTeam(renters + returners);
    }

    // 정확히 totalOps 개를 빌리고 반납할 때까지 돌린다. totalOps 는 BlockSize 의 배수여야 한다. 블록을 빌리는 스레드에
    // 나눠 주고 남는 것은 앞 스레드가 하나씩 더 맡는다(코어 수가 2의 거듭제곱이 아닌 PC 에서도 모든 조합이
    // OperationsPerInvoke 와 같은 횟수를 한다. Codex 리뷰 2차 R1, C++ 과 같은 나눔).
    public void Run(int totalOps, Func<T> rent, Action<T> giveBack)
    {
        if (totalOps <= 0 || totalOps % BlockSize != 0)
        {
            throw new ArgumentException("totalOps must be a positive multiple of 64", nameof(totalOps));
        }
        var totalBlocks = totalOps / BlockSize;
        _team.RunEach(t =>
        {
            if (t < _renters)
            {
                RunRenter(t, BlocksOfRenter(totalBlocks, t), rent);
            }
            else
            {
                RunReturner(t - _renters, totalBlocks, giveBack);
            }
        });
    }

    public void Dispose() => _team.Dispose();

    private RentReturnCell CellAt(int renter, int returner) => _cells[renter * _returners + returner];

    // 빌리는 스레드 renter 가 맡는 블록 수: 고르게 나누고 남는 것은 앞 스레드가 하나씩 더.
    private int BlocksOfRenter(int totalBlocks, int renter) =>
        totalBlocks / _renters + (renter < totalBlocks % _renters ? 1 : 0);

    // renter 가 returner 에게 보내는 블록 수(블록 번호 % M 으로 보내므로 앞 returner 가 하나씩 더 받는다).
    private int BlocksBetween(int totalBlocks, int renter, int returner)
    {
        var blocks = BlocksOfRenter(totalBlocks, renter);
        return blocks / _returners + (returner < blocks % _returners ? 1 : 0);
    }

    private void RunRenter(int renter, int blocks, Func<T> rent)
    {
        var next = new int[_returners]; // 쌍마다 다음에 채울 블록(0, 1 번갈아)
        for (var b = 0; b < blocks; ++b)
        {
            var returner = b % _returners; // 반납하는 쪽을 차례로 바꾼다
            var cell = CellAt(renter, returner);
            var block = (T?[])(next[returner] == 0 ? cell.Block0 : cell.Block1);
            for (var i = 0; i < block.Length; ++i)
            {
                block[i] = rent();
            }
            // 우편함이 비었다 = 이전에 넣은 블록을 다 반납했다. 그래서 블록 두 개를 번갈아 쓰면 겹치지 않는다
            while (Volatile.Read(ref cell.Mailbox) is not null)
            {
                Thread.Yield();
            }
            Volatile.Write(ref cell.Mailbox, block);
            next[returner] ^= 1;
        }
    }

    private void RunReturner(int returner, int totalBlocks, Action<T> giveBack)
    {
        var expected = 0;
        for (var renter = 0; renter < _renters; ++renter)
        {
            expected += BlocksBetween(totalBlocks, renter, returner);
        }
        var received = 0;
        while (received < expected)
        {
            var progressed = false;
            for (var renter = 0; renter < _renters; ++renter)
            {
                var cell = CellAt(renter, returner);
                if (Volatile.Read(ref cell.Mailbox) is not T?[] block)
                {
                    continue;
                }
                for (var i = 0; i < block.Length; ++i)
                {
                    giveBack(block[i]!);
                    block[i] = null;
                }
                Volatile.Write(ref cell.Mailbox, null);
                ++received;
                progressed = true;
            }
            if (!progressed)
            {
                Thread.Yield();
            }
        }
    }

}

// RentReturnGrid 의 쌍 하나. 객체들이 힙에 붙어 놓이므로 우편함을 64B 이상 띄워 이웃 쌍과 같은 캐시 줄에 놓이지 않게
// 한다. 제네릭 타입(제네릭 클래스 안의 중첩 타입 포함)은 명시적 레이아웃을 쓸 수 없어 밖에 두고, 블록을 object 로 들고
// 꺼낼 때 T?[] 로 바꾼다(블록 하나에 한 번).
[StructLayout(LayoutKind.Explicit, Size = 192)]
internal sealed class RentReturnCell
{
    [FieldOffset(64)]
    public object? Mailbox; // 비었으면 null

    [FieldOffset(128)]
    public readonly object Block0;

    [FieldOffset(136)]
    public readonly object Block1;

    public RentReturnCell(object block0, object block1)
    {
        Block0 = block0;
        Block1 = block1;
    }
}
