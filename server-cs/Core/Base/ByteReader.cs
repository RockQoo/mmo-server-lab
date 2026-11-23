using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Unicode;

namespace Base;

// 바이트를 값으로 읽는다. ByteWriter가 쓴 순서대로 읽어야 한다. 형식은 C++ Base::ByteReader와 같다.
//
// 받은 바이트를 복사하지 않고 ReadOnlySpan으로 가리키기만 한다(C++의 span과 같다).
// ref struct라서 원본보다 오래 살거나 await를 넘길 수 없다. C++에서는 주석으로 지키던 규칙을 컴파일러가 막는다.
//
// 데이터가 모자라거나, 길이가 남은 바이트보다 크거나, UTF-8이 잘못되면 실패 표시를 남기고 이후 읽기는 모두 무시한다.
// 실패한 값은 기본값(0, false, "")이 된다. 끝에서 IsValid로 한 번 확인한다.
public ref struct ByteReader
{
    public const int MaxCount = ByteWriter.MaxCount;

    private readonly ReadOnlySpan<byte> _data;
    private int _pos;
    private bool _valid;

    public ByteReader(ReadOnlySpan<byte> data)
    {
        // 리틀 엔디안 고정(ByteWriter 생성자 주석 참고)
        if (!BitConverter.IsLittleEndian)
        {
            throw new PlatformNotSupportedException("ByteReader는 리틀 엔디안 CPU만 지원한다");
        }
        _data = data;
        _valid = true;
    }

    public readonly bool IsValid => _valid;

    public readonly int ReadPos => _pos;

    public readonly int Remaining => _data.Length - _pos;

    public T Read<T>() where T : unmanaged, IBinaryInteger<T>
    {
        return ReadRaw<T>();
    }

    public T ReadEnum<T>() where T : unmanaged, Enum
    {
        return ReadRaw<T>();
    }

    // 0이 아니면 true(C++과 같다).
    public bool ReadBool()
    {
        return ReadRaw<byte>() != 0;
    }

    public float ReadSingle()
    {
        return ReadRaw<float>();
    }

    public double ReadDouble()
    {
        return ReadRaw<double>();
    }

    public string ReadString()
    {
        var length = ReadCount();
        if (!_valid)
        {
            return string.Empty;
        }

        var bytes = _data.Slice(_pos, length);
        // 잘못된 UTF-8은 대체 문자(U+FFFD)로 바꿔 받지 않고 실패로 본다(C++과 같은 검사).
        if (!Utf8.IsValid(bytes))
        {
            _valid = false;
            return string.Empty;
        }
        _pos += length;
        return Encoding.UTF8.GetString(bytes);
    }

    // 코덱으로 읽는다: 컨테이너, nullable, 클래스(ByteCodecs.cs). 예: reader.Read<int[], IntegerArrayCodec<int>>()
    public T Read<T, TCodec>() where TCodec : IByteCodec<T>
    {
        return TCodec.Read(ref this);
    }

    // 바이트를 그대로 채운다(정수 배열 통째 복사 등). 모자라면 실패 표시 후 false.
    public bool ReadBytes(Span<byte> destination)
    {
        if (!_valid || destination.Length > Remaining)
        {
            _valid = false;
            return false;
        }
        _data.Slice(_pos, destination.Length).CopyTo(destination);
        _pos += destination.Length;
        return true;
    }

    // 길이·개수를 읽는다. 원소 크기 × 개수가 남은 바이트보다 크면 실패 표시 후 0. 코덱과 CodeGen 이 쓴다.
    // 거짓 개수로 큰 메모리를 잡게 하는 패킷을 할당 전에 막는다. 원소 크기를 모르면(가변 길이) 최소 1바이트로 본다.
    public int ReadCount(int minElementSize = 1)
    {
        int count = ReadRaw<ushort>();
        if ((long)count * minElementSize > Remaining)
        {
            _valid = false;
            return 0;
        }
        return count;
    }

    private T ReadRaw<T>() where T : unmanaged
    {
        var size = Unsafe.SizeOf<T>();
        if (!_valid || size > Remaining)
        {
            _valid = false;
            return default;
        }
        var value = MemoryMarshal.Read<T>(_data.Slice(_pos, size));
        _pos += size;
        return value;
    }
}
