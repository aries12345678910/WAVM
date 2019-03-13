#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

#include "WAVM/IR/Module.h"
#include "WAVM/IR/Operators.h"
#include "WAVM/IR/Types.h"
#include "WAVM/IR/Validate.h"
#include "WAVM/IR/Value.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/Timing.h"
#include "WAVM/Logging/Logging.h"
#include "WAVM/Platform/Thread.h"
#include "WAVM/Runtime/Runtime.h"

enum
{
	numInvokesPerThread = 100000
};

using namespace WAVM;
using namespace WAVM::IR;
using namespace WAVM::Runtime;

struct ThreadArgs
{
	Context* context = nullptr;
	Function* nopFunction = nullptr;
	U64 elapsedMicroseconds = 0;
	Platform::Thread* thread = nullptr;
};

void runBenchmark(Compartment* compartment,
				  Function* nopFunction,
				  Uptr numThreads,
				  const char* description,
				  I64 (*threadFunc)(void*))
{
	// Create a thread for each hardware thread.
	std::vector<ThreadArgs*> threads;
	for(Uptr threadIndex = 0; threadIndex < numThreads; ++threadIndex)
	{
		ThreadArgs* threadArgs = new ThreadArgs;
		threadArgs->context = createContext(compartment);
		threadArgs->nopFunction = nopFunction;
		threadArgs->thread = Platform::createThread(512 * 1024, threadFunc, threadArgs);
		threads.push_back(threadArgs);
	}

	// Wait for the threads to exit, and sum the results from each thread.
	U64 totalElapsedMicroseconds = 0;
	for(ThreadArgs* threadArgs : threads)
	{
		Platform::joinThread(threadArgs->thread);
		totalElapsedMicroseconds += threadArgs->elapsedMicroseconds;
		delete threadArgs;
	}

	// Print the results.
	const U64 numCalls = U64(numInvokesPerThread) * numThreads;
	const F64 nanosecondsPerInvoke = F64(totalElapsedMicroseconds) / F64(numCalls) * 1000.0;

	Log::printf(Log::output,
				"ns/%s in %" PRIuPTR " threads: %.1f\n",
				description,
				numThreads,
				nanosecondsPerInvoke);
}

int main(int argc, char** argv)
{
	// Generate a nop function.
	Serialization::ArrayOutputStream codeStream;
	OperatorEncoderStream encoder(codeStream);
	encoder.end();

	// Generate a module containing the nop function.
	IR::Module irModule;
	DisassemblyNames irModuleNames;
	irModule.types.push_back(FunctionType());
	irModule.functions.defs.push_back({{0}, {}, std::move(codeStream.getBytes()), {}});
	irModule.exports.push_back({"nopFunction", IR::ExternKind::function, 0});
	irModuleNames.functions.push_back({"nopFunction", {}, {}});
	IR::setDisassemblyNames(irModule, irModuleNames);
	IR::validatePreCodeSections(irModule);
	IR::validatePostCodeSections(irModule);

	// Instantiate the module and return the stub function instance.
	GCPointer<Compartment> compartment = Runtime::createCompartment();
	auto module = compileModule(irModule);
	auto moduleInstance = instantiateModule(compartment, module, {}, "nopModule");
	auto nopFunction = asFunction(getInstanceExport(moduleInstance, "nopFunction"));

	// Call the nop function once to ensure the time to create the invoke thunk isn't benchmarked.
	invokeFunctionChecked(createContext(compartment), nopFunction, {});

	const Uptr numThreads = Platform::getNumberOfHardwareThreads();

	runBenchmark(
		compartment, nopFunction, numThreads, "invokeFunctionUnchecked", [](void* argument) -> I64 {
			ThreadArgs* threadArgs = (ThreadArgs*)argument;

			// Measure the time it takes to call the nop function many times.
			Timing::Timer timer;
			for(Uptr repeatIndex = 0; repeatIndex < numInvokesPerThread; ++repeatIndex)
			{ invokeFunctionUnchecked(threadArgs->context, threadArgs->nopFunction, nullptr); }
			timer.stop();

			threadArgs->elapsedMicroseconds = timer.getMicroseconds();

			return 0;
		});

	runBenchmark(compartment, nopFunction, 1, "invokeFunctionUnchecked", [](void* argument) -> I64 {
		ThreadArgs* threadArgs = (ThreadArgs*)argument;

		// Measure the time it takes to call the nop function many times.
		Timing::Timer timer;
		for(Uptr repeatIndex = 0; repeatIndex < numInvokesPerThread; ++repeatIndex)
		{ invokeFunctionUnchecked(threadArgs->context, threadArgs->nopFunction, nullptr); }
		timer.stop();

		threadArgs->elapsedMicroseconds = timer.getMicroseconds();

		return 0;
	});

	runBenchmark(
		compartment, nopFunction, numThreads, "invokeFunctionChecked", [](void* argument) -> I64 {
			ThreadArgs* threadArgs = (ThreadArgs*)argument;

			// Measure the time it takes to call the nop function many times.
			Timing::Timer timer;
			for(Uptr repeatIndex = 0; repeatIndex < numInvokesPerThread; ++repeatIndex)
			{ invokeFunctionChecked(threadArgs->context, threadArgs->nopFunction, {}); }
			timer.stop();

			threadArgs->elapsedMicroseconds = timer.getMicroseconds();

			return 0;
		});

	runBenchmark(compartment, nopFunction, 1, "invokeFunctionChecked", [](void* argument) -> I64 {
		ThreadArgs* threadArgs = (ThreadArgs*)argument;

		// Measure the time it takes to call the nop function many times.
		Timing::Timer timer;
		for(Uptr repeatIndex = 0; repeatIndex < numInvokesPerThread; ++repeatIndex)
		{ invokeFunctionChecked(threadArgs->context, threadArgs->nopFunction, {}); }
		timer.stop();

		threadArgs->elapsedMicroseconds = timer.getMicroseconds();

		return 0;
	});

	// Free the compartment.
	errorUnless(tryCollectCompartment(std::move(compartment)));

	return 0;
}