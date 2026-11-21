using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

internal static class Program
{
    private readonly record struct NameQuery(int Table, string Key);
    private static bool report;
    private static int? requestedCapacity;

    [DllImport("kernel32.dll")]
    private static extern nint GetCurrentThread();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nuint SetThreadAffinityMask(nint thread, nuint mask);

    private static string ReadText(BinaryReader input)
    {
        int size = input.ReadInt32();
        byte[] bytes = input.ReadBytes(size);
        if (bytes.Length != size) throw new EndOfStreamException();
        return Encoding.ASCII.GetString(bytes);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ulong Sweep(Dictionary<ulong, uint> table, ulong[] keys)
    {
        ulong sum = 0;
        foreach (ulong key in keys)
        {
            table.TryGetValue(key, out uint value);
            sum += value;
        }
        return sum;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ulong Sweep(Dictionary<string, uint>[] tables, NameQuery[] queries)
    {
        ulong sum = 0;
        foreach (ref readonly var query in queries.AsSpan())
        {
            tables[query.Table].TryGetValue(query.Key, out uint value);
            sum += value;
        }
        return sum;
    }

    private static void Measure(string metric, int queries, ulong expected, Func<ulong> sweep)
    {
        long warmStart = Stopwatch.GetTimestamp();
        do
        {
            if (sweep() != expected) throw new InvalidDataException(metric);
        } while (Stopwatch.GetElapsedTime(warmStart).TotalMilliseconds < 150);

        const int passes = 32;
        ulong total = 0;
        int collections = GC.CollectionCount(0);
        long allocated = GC.GetAllocatedBytesForCurrentThread();
        long start = Stopwatch.GetTimestamp();
        for (int i = 0; i < passes; i++) total += sweep();
        double ns = Stopwatch.GetElapsedTime(start).TotalNanoseconds / (queries * passes);
        long allocatedBytes = GC.GetAllocatedBytesForCurrentThread() - allocated;
        collections = GC.CollectionCount(0) - collections;
        if (total != passes * expected || allocatedBytes != 0) throw new InvalidDataException(metric);
        if (report) Console.WriteLine($"{metric},{ns:F5},{expected},{collections}");
    }

    private static void IntegerCase(BinaryReader input, string metric)
    {
        int count = input.ReadInt32();
        if (requestedCapacity.HasValue && count != 65536) throw new InvalidDataException(metric);
        long allocated = GC.GetAllocatedBytesForCurrentThread();
        var table = new Dictionary<ulong, uint>(requestedCapacity ?? count);
        long tableBytes = GC.GetAllocatedBytesForCurrentThread() - allocated;
        if (report && requestedCapacity.HasValue && metric == "dense_65536_hit")
            Console.Error.WriteLine($"capacity={table.EnsureCapacity(0)}, allocated bytes={tableBytes}");
        for (int i = 0; i < count; i++) table.Add(input.ReadUInt64(), (uint)i + 1);
        var keys = new ulong[input.ReadInt32()];
        ulong expected = 0;
        for (int i = 0; i < keys.Length; i++)
        {
            keys[i] = input.ReadUInt64();
            uint value = input.ReadUInt32();
            table.TryGetValue(keys[i], out uint actual);
            if (actual != value) throw new InvalidDataException(metric);
            expected += value;
        }
        Measure(metric, keys.Length, expected, () => Sweep(table, keys));
    }

    private static void NameCase(BinaryReader input, string metric)
    {
        var tables = new Dictionary<string, uint>[input.ReadInt32()];
        for (int t = 0; t < tables.Length; t++)
        {
            int size = input.ReadInt32();
            tables[t] = new Dictionary<string, uint>(size, StringComparer.Ordinal);
            for (int i = 0; i < size; i++) tables[t].Add(ReadText(input), (uint)i + 1);
        }
        var queries = new NameQuery[input.ReadInt32()];
        ulong expected = 0;
        for (int i = 0; i < queries.Length; i++)
        {
            var query = new NameQuery(input.ReadInt32(), ReadText(input));
            queries[i] = query;
            uint value = input.ReadUInt32();
            tables[query.Table].TryGetValue(query.Key, out uint actual);
            if (actual != value) throw new InvalidDataException(metric);
            expected += value;
        }
        Measure(metric, queries.Length, expected, () => Sweep(tables, queries));
    }

    private static void Replay(string path)
    {
        using var input = new BinaryReader(File.OpenRead(path));
        int count = input.ReadInt32();
        for (int i = 0; i < count; i++)
        {
            string metric = ReadText(input);
            switch (input.ReadUInt32())
            {
                case 0: IntegerCase(input, metric); break;
                case 1: NameCase(input, metric); break;
                default: throw new InvalidDataException(metric);
            }
        }
        if (input.BaseStream.Position != input.BaseStream.Length) throw new InvalidDataException();
    }

    private static void Main(string[] args)
    {
        CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
        int cpu = int.Parse(args[1]);
        if (args.Length == 3) requestedCapacity = int.Parse(args[2]);
        if ((uint)cpu >= 64 || SetThreadAffinityMask(GetCurrentThread(), (nuint)1 << cpu) == 0)
            throw new InvalidOperationException("Could not pin the measuring thread.");
        Replay(args[0]);
        report = true;
        Replay(args[0]);
    }
}
