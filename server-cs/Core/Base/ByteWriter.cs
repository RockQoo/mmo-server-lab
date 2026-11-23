using System.Buffers;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Unicode;

namespace Base;

// 값을 바이트로 쓴다. 형식은 C++ Base::ByteWriter와 같다(리틀 엔디안, 길이 = uint16 + UTF-8).
//
// 버퍼는 직접 갖지 않고 .NET의 IBufferWriter<byte>에 쓴다.
//   테스트·내부 메시지: ArrayBufferWriter<byte>    네트워크: PipeWriter(소켓 버퍼에 바로 쓴다, 복사 없음)
// 그래서 이 타입은 "어디까지 썼나"만 다루는 짧게 쓰는 커서다. ref struct라 힙에 할당되지 않고(패킷마다 GC 할당 0),
// 필드에 저장하거나 await를 넘겨 들고 다닐 수 없다(컴파일 에러).
//
// 실패(길이 초과, 잘못된 UTF-16)하면 실패 표시를 남기고 이후 쓰기는 모두 무시한다. 보내기 전에 IsValid로 확인한다.
// 예외를 쓰지 않는 이유: 패킷마다 도는 경로라 예외 비용을 피하고, C++과 같은 방식으로 확인하게 한다.
public ref struct ByteWriter
{
    // string 길이, 컨테이너 개수의 상한(uint16)
    public const int MaxCount = ushort.MaxValue;

    private readonly IBufferWriter<byte> _output;
    private int _written;
    private bool _valid;

    public ByteWriter(IBufferWriter<byte> output)
    {
        // 형식은 리틀 엔디안 고정. 메모리를 그대로 복사하므로 빅 엔디안 CPU에서는 형식이 깨진다.
        // JIT 가 상수로 접어 x64 에서는 코드가 남지 않는다. 정적 생성자에서 던지면 타입 전체가 못 쓰게 되어(CA1065) 여기서 본다.
        if (!BitConverter.IsLittleEndian)
        {
            throw new PlatformNotSupportedException("ByteWriter는 리틀 엔디안 CPU만 지원한다");
        }
        _output = output;
        _valid = true;
    }

    public readonly bool IsValid => _valid;

    // 이 커서로 쓴 바이트 수
    public readonly int Written => _written;

    // 정수 전부(sbyte ~ ulong, char). 지원하지 않는 타입은 제약 조건에 걸려 컴파일 에러(C++의 static_assert 자리).
    public void Write<T>(T value) where T : unmanaged, IBinaryInteger<T>
    {
        WriteRaw(value);
    }

    // enum은 바탕 타입 크기 그대로(C++ enum class : uint8_t 와 같은 바이트). 제약이 달라도 이름이 같으면 겹쳐서 이름을 나눴다.
    public void WriteEnum<T>(T value) where T : unmanaged, Enum
    {
        WriteRaw(value);
    }

    public void Write(bool value)
    {
        Write<byte>(value ? (byte)1 : (byte)0);
    }

    public void Write(float value)
    {
        WriteRaw(value);
    }

    public void Write(double value)
    {
        WriteRaw(value);
    }

    // uint16 길이(UTF-8 바이트 수) + UTF-8. 짝이 없는 서로게이트(잘못된 UTF-16)는 바꿔 쓰지 않고 실패로 본다.
    public void Write(string value)
    {
        // UTF-16 한 글자는 UTF-8로 최소 1바이트라 글자 수가 이미 넘으면 볼 필요가 없다.
        // null 은 예외 대신 실패로 본다(nullable 주석을 무시하고 들어온 null, C++ 의 null C 문자열과 같은 처리).
        if (!_valid || value is null || value.Length > MaxCount)
        {
            _valid = false;
            return;
        }

        // 1) 바이트 수를 먼저 센다(SIMD로 빠르다). 잘못된 UTF-16은 대체 문자 크기로 세지므로 올바른 문자열이면 정확하다.
        //    최대 크기(글자당 3바이트)로 빌리면 긴 ASCII 문자열에서 버퍼를 3배로 키우게 된다.
        var count = Encoding.UTF8.GetByteCount(value);
        if (count > MaxCount)
        {
            _valid = false;
            return;
        }

        // 2) 딱 그만큼 빌려 바꾼다. 짝 없는 서로게이트는 여기서 InvalidData로 걸린다. 중간 배열 할당이 없다.
        var span = _output.GetSpan(sizeof(ushort) + count);
        var status = Utf8.FromUtf16(value, span.Slice(sizeof(ushort), count), out _, out var length,
                                    replaceInvalidSequences: false);
        if (status != OperationStatus.Done)
        {
            _valid = false;
            return;
        }

        MemoryMarshal.Write(span, (ushort)length);
        Advance(sizeof(ushort) + length);
    }

    // 코덱으로 쓴다: 컨테이너, nullable, 클래스(ByteCodecs.cs). 예: writer.Write<int[], IntegerArrayCodec<int>>(ids)
    public void Write<T, TCodec>(T value) where TCodec : IByteCodec<T>
    {
        TCodec.Write(ref this, value);
    }

    // 실패 표시만 남긴다. 코덱이 쓸 수 없는 값(null 클래스·배열 등)을 만났을 때 쓴다. 이후 쓰기는 모두 무시된다.
    public void Fail()
    {
        _valid = false;
    }

    // 컨테이너 개수(uint16). 상한(MaxCount)을 넘거나 음수면 실패 표시 후 false. 코덱과 CodeGen 이 쓴다.
    public bool WriteCount(int count)
    {
        if (!_valid || count < 0 || count > MaxCount)
        {
            _valid = false;
            return false;
        }
        Write((ushort)count);
        return _valid;
    }

    // 바이트를 그대로 쓴다(정수 배열 통째 복사 등).
    public void WriteBytes(ReadOnlySpan<byte> bytes)
    {
        if (!_valid || bytes.IsEmpty)
        {
            return;
        }
        bytes.CopyTo(_output.GetSpan(bytes.Length));
        Advance(bytes.Length);
    }

    private void WriteRaw<T>(T value) where T : unmanaged
    {
        if (!_valid)
        {
            return;
        }
        var size = Unsafe.SizeOf<T>();
        MemoryMarshal.Write(_output.GetSpan(size), in value);
        Advance(size);
    }

    private void Advance(int count)
    {
        _output.Advance(count);
        _written += count;
    }
}
