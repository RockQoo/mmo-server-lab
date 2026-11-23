using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Base;

// 타입 하나를 쓰고 읽는 방법(코덱, codec: 인코더 + 디코더). C++ ByteWriter 의 if constexpr 판별을 C# 에서는 이렇게 푼다.
//
// static abstract 인터페이스(C# 11)라 객체를 만들지 않고, 코덱을 struct 로 두면 JIT(Just-In-Time 컴파일러)가 코덱 타입마다
// 코드를 따로 만들어 호출이 인라인된다(가상 호출·delegate 비용 없음). 코덱끼리 조합해 중첩 컨테이너도 된다:
//   writer.Write<List<int?>, ListCodec<int?, NullableCodec<int, IntegerCodec<int>>>>(values);
// 손으로 쓰면 길지만, 패킷·데이터 클래스는 CodeGen 이 이 코드를 만든다.
public interface IByteCodec<T>
{
    static abstract void Write(ref ByteWriter writer, T value);
    static abstract T Read(ref ByteReader reader);
}

// ---- 기본형 ----

public readonly struct IntegerCodec<T> : IByteCodec<T> where T : unmanaged, IBinaryInteger<T>
{
    public static void Write(ref ByteWriter writer, T value) => writer.Write(value);
    public static T Read(ref ByteReader reader) => reader.Read<T>();
}

public readonly struct EnumCodec<T> : IByteCodec<T> where T : unmanaged, Enum
{
    public static void Write(ref ByteWriter writer, T value) => writer.WriteEnum(value);
    public static T Read(ref ByteReader reader) => reader.ReadEnum<T>();
}

public readonly struct BoolCodec : IByteCodec<bool>
{
    public static void Write(ref ByteWriter writer, bool value) => writer.Write(value);
    public static bool Read(ref ByteReader reader) => reader.ReadBool();
}

public readonly struct SingleCodec : IByteCodec<float>
{
    public static void Write(ref ByteWriter writer, float value) => writer.Write(value);
    public static float Read(ref ByteReader reader) => reader.ReadSingle();
}

public readonly struct DoubleCodec : IByteCodec<double>
{
    public static void Write(ref ByteWriter writer, double value) => writer.Write(value);
    public static double Read(ref ByteReader reader) => reader.ReadDouble();
}

public readonly struct StringCodec : IByteCodec<string>
{
    public static void Write(ref ByteWriter writer, string value) => writer.Write(value);
    public static string Read(ref ByteReader reader) => reader.ReadString();
}

// ---- 값이 있을 수도 없을 수도 (C++ std::optional: bool(있음) + 값) ----

// 값 형식: int?, MyStruct? (Nullable<T>)
public readonly struct NullableCodec<T, TCodec> : IByteCodec<T?> where T : struct where TCodec : IByteCodec<T>
{
    public static void Write(ref ByteWriter writer, T? value)
    {
        writer.Write(value.HasValue);
        if (value.HasValue)
        {
            TCodec.Write(ref writer, value.GetValueOrDefault());
        }
    }

    // 안의 값을 읽다 실패하면 값이 아니라 null 을 돌려준다(C++ 처럼 실패한 optional 은 비운다)
    public static T? Read(ref ByteReader reader)
    {
        if (!reader.ReadBool())
        {
            return null;
        }
        var value = TCodec.Read(ref reader);
        return reader.IsValid ? value : null;
    }
}

// 참조 형식: string?, MyClass?
public readonly struct NullableRefCodec<T, TCodec> : IByteCodec<T?> where T : class where TCodec : IByteCodec<T>
{
    public static void Write(ref ByteWriter writer, T? value)
    {
        writer.Write(value is not null);
        if (value is not null)
        {
            TCodec.Write(ref writer, value);
        }
    }

    // 안의 값을 읽다 실패하면 값이 아니라 null 을 돌려준다(C++ 처럼 실패한 optional 은 비운다)
    public static T? Read(ref ByteReader reader)
    {
        if (!reader.ReadBool())
        {
            return null;
        }
        var value = TCodec.Read(ref reader);
        return reader.IsValid ? value : null;
    }
}

// ---- 컨테이너 (C++ vector 등: uint16 개수 + 원소) ----

public readonly struct ArrayCodec<T, TCodec> : IByteCodec<T[]> where TCodec : IByteCodec<T>
{
    public static void Write(ref ByteWriter writer, T[] value) => WriteItems<T, TCodec>(ref writer, value);

    public static T[] Read(ref ByteReader reader)
    {
        var count = reader.ReadCount();
        var items = new T[count];
        for (var i = 0; i < count && reader.IsValid; i++)
        {
            items[i] = TCodec.Read(ref reader);
        }
        return reader.IsValid ? items : [];
    }

    internal static void WriteItems<TItem, TItemCodec>(ref ByteWriter writer, ReadOnlySpan<TItem> items)
        where TItemCodec : IByteCodec<TItem>
    {
        if (!writer.WriteCount(items.Length))
        {
            return;
        }
        foreach (var item in items)
        {
            TItemCodec.Write(ref writer, item);
        }
    }
}

public readonly struct ListCodec<T, TCodec> : IByteCodec<List<T>> where TCodec : IByteCodec<T>
{
    // List 의 내부 배열을 복사 없이 span 으로 본다(쓰는 동안 List 를 바꾸지 않는다는 전제)
    public static void Write(ref ByteWriter writer, List<T> value) =>
        ArrayCodec<T, TCodec>.WriteItems<T, TCodec>(ref writer, CollectionsMarshal.AsSpan(value));

    public static List<T> Read(ref ByteReader reader)
    {
        var count = reader.ReadCount();
        var items = new List<T>(count);
        for (var i = 0; i < count && reader.IsValid; i++)
        {
            items.Add(TCodec.Read(ref reader));
        }
        return reader.IsValid ? items : [];
    }
}

// 정수 배열 전용: 원소를 하나씩 쓰지 않고 메모리를 통째로 복사한다(memcpy 한 번). 바이트는 ArrayCodec 과 같다.
public readonly struct IntegerArrayCodec<T> : IByteCodec<T[]> where T : unmanaged, IBinaryInteger<T>
{
    public static void Write(ref ByteWriter writer, T[] value)
    {
        if (writer.WriteCount(value.Length))
        {
            writer.WriteBytes(MemoryMarshal.AsBytes(value.AsSpan()));
        }
    }

    public static T[] Read(ref ByteReader reader)
    {
        // 원소 크기까지 계산해 배열을 만들기 전에 거짓 개수를 막는다(long 65535개 = 512KB 할당을 데이터 없이 시키지 못하게)
        var count = reader.ReadCount(Unsafe.SizeOf<T>());
        if (!reader.IsValid)
        {
            return [];
        }
        var items = new T[count];
        return reader.ReadBytes(MemoryMarshal.AsBytes(items.AsSpan())) ? items : [];
    }
}
