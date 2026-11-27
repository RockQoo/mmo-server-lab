using System.Buffers;
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

// ---- 통째 복사(blittable) 판별. C++ Base::Blittable 자리 ----

// 코덱이 필드마다 쓴 바이트가 T 의 메모리 그대로와 같은지(true 면 배열·List 를 memcpy 한 번으로 쓰고 읽는다).
// C# 은 코덱이 필드를 쓰는 순서를 컴파일할 때 알 수 없어서, 타입마다 처음 한 번 실제로 써 보고 비교한다:
// T 의 메모리를 서로 다른 바이트(2, 3, 4 ...)로 채우고 코덱으로 쓴 결과·코덱으로 읽은 결과가 그 메모리와 같은지.
// 순서가 다르거나, 빈틈·빠진 필드가 있거나, bool(1 로 바꿔 쓴다)이 있으면 달라서 false 다. 2 부터 채우는 것은 bool 자리에
// 0·1 이 와서 우연히 같아지지 않게, 무늬 두 개(주기 200, 199)는 멀리 떨어진 같은 크기 필드가 바뀐 것도 잡으려는 것이다.
// 참조가 있는 타입(클래스, string 이 든 struct)은 늘 false. 결과는 static readonly 라 JIT 가 상수로 접는다.
public static class Blittable<T, TCodec> where TCodec : IByteCodec<T>
{
    public static readonly bool IsBlittable = Check();

    private static bool Check() =>
        !RuntimeHelpers.IsReferenceOrContainsReferences<T>() && Matches(period: 200) && Matches(period: 199);

    private static bool Matches(int period)
    {
        var size = Unsafe.SizeOf<T>();
        var pattern = new byte[size];
        for (var i = 0; i < size; i++)
        {
            pattern[i] = (byte)((i % period) + 2);
        }

        var output = new ArrayBufferWriter<byte>(size);
        var writer = new ByteWriter(output);
        TCodec.Write(ref writer, Unsafe.ReadUnaligned<T>(ref pattern[0]));
        if (!writer.IsValid || !output.WrittenSpan.SequenceEqual(pattern))
        {
            return false;
        }

        var reader = new ByteReader(pattern);
        var read = TCodec.Read(ref reader);
        if (!reader.IsValid || reader.Remaining != 0)
        {
            return false;
        }
        var back = new byte[size];
        Unsafe.WriteUnaligned(ref back[0], read);
        return back.AsSpan().SequenceEqual(pattern);
    }
}

// Blittable 이 true 인 타입의 span 을 바이트로 본다(복사 없음)
internal static class BlittableBytes
{
    public static ReadOnlySpan<byte> Of<T>(ReadOnlySpan<T> items) =>
        MemoryMarshal.CreateReadOnlySpan(ref Unsafe.As<T, byte>(ref MemoryMarshal.GetReference(items)),
                                         items.Length * Unsafe.SizeOf<T>());

    public static Span<byte> Of<T>(Span<T> items) =>
        MemoryMarshal.CreateSpan(ref Unsafe.As<T, byte>(ref MemoryMarshal.GetReference(items)),
                                 items.Length * Unsafe.SizeOf<T>());
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

// ---- ID (C++ StrongId: 정수 하나와 같은 바이트) ----

public readonly struct RuidCodec : IByteCodec<Ruid>
{
    public static void Write(ref ByteWriter writer, Ruid value) => writer.Write(value.Value);
    public static Ruid Read(ref ByteReader reader) => new(reader.Read<long>());
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
    public static void Write(ref ByteWriter writer, T[] value)
    {
        if (value is null)
        {
            writer.Fail(); // null 은 예외 대신 실패(빈 배열과 구분한다)
            return;
        }
        WriteItems<T, TCodec>(ref writer, value);
    }

    public static T[] Read(ref ByteReader reader)
    {
        if (Blittable<T, TCodec>.IsBlittable)
        {
            // 원소 크기까지 계산해 배열을 만들기 전에 거짓 개수를 막는다(IntegerArrayCodec 과 같다)
            var blockCount = reader.ReadCount(Unsafe.SizeOf<T>());
            if (!reader.IsValid)
            {
                return [];
            }
            var block = new T[blockCount];
            return reader.ReadBytes(BlittableBytes.Of(block.AsSpan())) ? block : [];
        }

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
        if (Blittable<TItem, TItemCodec>.IsBlittable)
        {
            writer.WriteBytes(BlittableBytes.Of(items)); // 원소마다 쓰지 않고 한 번에(바이트는 같다)
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
    public static void Write(ref ByteWriter writer, List<T> value)
    {
        if (value is null)
        {
            writer.Fail();
            return;
        }
        ArrayCodec<T, TCodec>.WriteItems<T, TCodec>(ref writer, CollectionsMarshal.AsSpan(value));
    }

    public static List<T> Read(ref ByteReader reader)
    {
        if (Blittable<T, TCodec>.IsBlittable)
        {
            var blockCount = reader.ReadCount(Unsafe.SizeOf<T>());
            if (!reader.IsValid)
            {
                return [];
            }
            var block = new List<T>(blockCount);
            CollectionsMarshal.SetCount(block, blockCount);
            return reader.ReadBytes(BlittableBytes.Of(CollectionsMarshal.AsSpan(block))) ? block : [];
        }

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
        if (value is null)
        {
            writer.Fail();
            return;
        }
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

// 키-값 컨테이너(C++ map: uint16 개수 + (키, 값) 쌍). 순서는 Dictionary 가 돌려주는 순서(추가한 순서).
// C++ std::map 은 키 순으로 쓰므로, 같은 바이트가 필요하면 키 순으로 넣거나 SortedDictionary 를 쓴다.
// 읽을 때 같은 키가 또 오면 먼저 온 값을 유지한다(C++ map 읽기와 같다).
public readonly struct DictionaryCodec<TKey, TValue, TKeyCodec, TValueCodec> : IByteCodec<Dictionary<TKey, TValue>>
    where TKey : notnull
    where TKeyCodec : IByteCodec<TKey>
    where TValueCodec : IByteCodec<TValue>
{
    public static void Write(ref ByteWriter writer, Dictionary<TKey, TValue> value)
    {
        if (value is null)
        {
            writer.Fail();
            return;
        }
        if (!writer.WriteCount(value.Count))
        {
            return;
        }
        foreach (var (key, item) in value)
        {
            TKeyCodec.Write(ref writer, key);
            TValueCodec.Write(ref writer, item);
        }
    }

    public static Dictionary<TKey, TValue> Read(ref ByteReader reader)
    {
        // 쌍은 최소 1바이트로 본다. 키나 값 한쪽은 0바이트일 수 있다(C++ map 도 허용). 둘 다 0바이트인 쌍은 C++ 처럼 지원하지 않는다.
        var count = reader.ReadCount();
        var items = new Dictionary<TKey, TValue>(count);
        for (var i = 0; i < count && reader.IsValid; i++)
        {
            var key = TKeyCodec.Read(ref reader);
            var item = TValueCodec.Read(ref reader);
            if (reader.IsValid)
            {
                items.TryAdd(key, item);
            }
        }
        return reader.IsValid ? items : [];
    }
}
