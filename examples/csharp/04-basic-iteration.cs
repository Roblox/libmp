// 04-basic-iteration.cs -- Iterating Scopes (Single Frame, Single Thread)
//
// Setup (.NET 8 SDK):
// - Download LibMP.cs from https://github.com/Roblox/libmp/releases, or build it
//   from the repository root with `python3 src/scripts/build.py --csharp`
//   (output: src/build/csharp/LibMP.cs).
// - Put LibMP.cs and this file in an empty directory with LibMPExample.csproj.
//   It references the Wasmtime NuGet package, the only dependency of LibMP.cs:
//
//   <Project Sdk="Microsoft.NET.Sdk">
//     <PropertyGroup>
//       <OutputType>Exe</OutputType>
//       <TargetFramework>net8.0</TargetFramework>
//       <Nullable>enable</Nullable>
//       <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
//     </PropertyGroup>
//     <ItemGroup>
//       <PackageReference Include="Wasmtime" Version="44.0.0" />
//     </ItemGroup>
//   </Project>
//
// - Run from that directory: dotnet run -- /path/to/capture.x.html (or capture.gprx)
//
// This file is part of LibMP and is licensed under the MIT License; see LICENSE for details.

using System;
using System.Collections.Generic;
using LibMP;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: dotnet run -- <capture.x.html|capture.gprx>");
    return 2;
}

using var libmp = LibMPInstance.Create();
using var session = libmp.OpenFromFile(args[0]);
if (session is null || !session.IsValid())
{
    Console.Error.WriteLine("Failed to open session");
    return 1;
}

uint frameId = session.GetFrameIdMax();
if (frameId == 0)
{
    Console.WriteLine("No frames available");
    return 0;
}

uint mainThreadId = session.FindThreadId("*rbx main*");
if (mainThreadId == 0)
{
    Console.WriteLine("No Main thread found");
    return 0;
}

var globalDesc = session.FetchGlobalDesc()
    ?? throw new InvalidOperationException("Global descriptor is missing");
var threadDesc = session.FetchThreadDesc(mainThreadId)
    ?? throw new InvalidOperationException("Thread descriptor is missing");
Console.WriteLine($"Frame {frameId}, Thread: '{threadDesc.ThreadName}'");
Console.WriteLine(new string('-', 60));

using var iterator = session.CreateLogIterator();
if (!iterator.Configure(new MpTypes.LogIteratorConfigT
    {
        ThreadIds = new List<uint> { mainThreadId },
    }))
{
    throw new InvalidOperationException("Failed to configure log iterator");
}

var state = iterator.GetState()
    ?? throw new InvalidOperationException("Log iterator state is missing");
iterator.RewindTo(frameId, frameId);

const int scopeLimit = 50;
int scopeCounter = 0;
while (iterator.Step())
{
    var timer = session.FetchTimerDesc(state.TimerId);
    string timerName = timer?.TimerName ?? $"<timer {state.TimerId}>";
    if (state.IsEnter)
    {
        Console.WriteLine($"{new string(' ', (int)state.ThreadStackDepth * 2)}> {timerName}");
    }
    else if (state.IsExit)
    {
        if (++scopeCounter > scopeLimit)
            break;

        uint depth = state.ThreadStackDepth;
        string duration = "";
        if (!state.ThreadStackIsUnderflowed)
        {
            var element = iterator.GetCurrentThreadStackElement((int)depth);
            if (element.HasValue)
            {
                double durationMs =
                    (state.Timestamp - element.Value.EnterTimestamp) * globalDesc.TickToMsCpu;
                duration = $" ({durationMs:F3} ms)";
            }
        }
        Console.WriteLine($"{new string(' ', ((int)depth + 1) * 2)}< {timerName}{duration}");
    }
}

Console.WriteLine("\nDone.");
return 0;
