/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <test/tools/fuzzer_common.h>

#include <libsolidity/interface/CompilerStack.h>

#include <libsolutil/JSON.h>

#include <libevmasm/Assembly.h>
#include <libevmasm/ConstantOptimiser.h>

#include <libsolc/libsolc.h>

#include <liblangutil/Exceptions.h>

#include <libyul/AssemblyStack.h>

#include <test/tools/ossfuzz/yulFuzzerCommon.h>

#include <liblangutil/SourceReferenceFormatter.h>
#include <sstream>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::evmasm;
using namespace solidity::langutil;
using namespace solidity::yul;
using namespace solidity::yul::test::yul_fuzzer;

namespace
{
void printErrors(ostream& _stream, ErrorList const& _errors)
{
	SourceReferenceFormatter formatter(_stream);

	for (auto const& error: _errors)
		formatter.printExceptionInformation(
			*error,
			(error->type() == Error::Type::Warning) ? "Warning" : "Error"
		);
}
}

static vector<EVMVersion> s_evmVersions = {
	EVMVersion::homestead(),
	EVMVersion::tangerineWhistle(),
	EVMVersion::spuriousDragon(),
	EVMVersion::byzantium(),
	EVMVersion::constantinople(),
	EVMVersion::petersburg(),
	EVMVersion::istanbul(),
	EVMVersion::berlin()
};

void FuzzerUtil::testCompilerJsonInterface(string const& _input, bool _optimize, bool _quiet)
{
	if (!_quiet)
		cout << "Testing compiler " << (_optimize ? "with" : "without") << " optimizer." << endl;

	Json::Value config = Json::objectValue;
	config["language"] = "Solidity";
	config["sources"] = Json::objectValue;
	config["sources"][""] = Json::objectValue;
	config["sources"][""]["content"] = _input;
	config["settings"] = Json::objectValue;
	config["settings"]["optimizer"] = Json::objectValue;
	config["settings"]["optimizer"]["enabled"] = _optimize;
	config["settings"]["optimizer"]["runs"] = 200;
	config["settings"]["evmVersion"] = "berlin";

	// Enable all SourceUnit-level outputs.
	config["settings"]["outputSelection"]["*"][""][0] = "*";
	// Enable all Contract-level outputs.
	config["settings"]["outputSelection"]["*"]["*"][0] = "*";

	runCompiler(jsonCompactPrint(config), _quiet);
}

void FuzzerUtil::yulIRDiff(EVMVersion _version, string const& _ir, string const& _irOpt)
{
	YulStringRepository::reset();

	if (_ir.empty() && _irOpt.empty())
		return;

	// AssemblyStack entry point
	AssemblyStack stackIr(
		_version,
		AssemblyStack::Language::StrictAssembly,
		solidity::frontend::OptimiserSettings::full()
	);

	// Parse protobuf mutated YUL code
	if (
		!stackIr.parseAndAnalyze("source", _ir) ||
		!stackIr.parserResult()->code ||
		!stackIr.parserResult()->analysisInfo ||
		!Error::containsOnlyWarnings(stackIr.errors())
	)
	{
		std::cout << _ir << std::endl;
		printErrors(std::cout, stackIr.errors());
		yulAssert(false, "Compiler generated malformed IR");
	}

	AssemblyStack stackIrOpt(
		_version,
		AssemblyStack::Language::StrictAssembly,
		solidity::frontend::OptimiserSettings::full()
	);

	// Parse protobuf mutated YUL code
	if (
		!stackIrOpt.parseAndAnalyze("source", _irOpt) ||
		!stackIrOpt.parserResult()->code ||
		!stackIrOpt.parserResult()->analysisInfo ||
		!Error::containsOnlyWarnings(stackIrOpt.errors())
		)
	{
		std::cout << _irOpt << std::endl;
		printErrors(std::cout, stackIrOpt.errors());
		yulAssert(false, "Compiler generated malformed optimized IR");
	}

	ostringstream os1;
	ostringstream os2;
	yulFuzzerUtil::interpret(
		os1,
		stackIr.parserResult()->code,
		EVMDialect::strictAssemblyForEVMObjects(_version)
	);

	yulFuzzerUtil::TerminationReason termReason = yulFuzzerUtil::interpret(
		os2,
		stackIrOpt.parserResult()->code,
		EVMDialect::strictAssemblyForEVMObjects(_version)
	);

	if (termReason == yulFuzzerUtil::TerminationReason::StepLimitReached)
		return;

	bool isTraceEq = (os1.str() == os2.str());
	yulAssert(isTraceEq, "Interpreted traces for optimized and unoptimized code differ.");
}

void FuzzerUtil::testCompiler(StringMap const& _input, bool _optimize, unsigned _rand)
{
	frontend::CompilerStack compiler;
	EVMVersion evmVersion = s_evmVersions[_rand % s_evmVersions.size()];
	frontend::OptimiserSettings optimiserSettings;
	if (_optimize)
		optimiserSettings = frontend::OptimiserSettings::standard();
	else
		optimiserSettings = frontend::OptimiserSettings::minimal();
	compiler.setSources(_input);
	compiler.enableIRGeneration();
	compiler.setEVMVersion(evmVersion);
	compiler.setOptimiserSettings(optimiserSettings);
	try
	{
		if (compiler.compile() && !compiler.contractNames().empty())
		{
			string lastContractName = compiler.lastContractName();
			yulIRDiff(
				evmVersion,
				compiler.yulIR(lastContractName),
				compiler.yulIROptimized(lastContractName)
			);
		}
	}
	catch (InternalCompilerError const&)
	{
	}
	catch (Error const&)
	{
	}
	catch (FatalError const&)
	{
	}
	catch (UnimplementedFeatureError const&)
	{
	}
	catch (StackTooDeepError const&)
	{
	}
}

void FuzzerUtil::runCompiler(string const& _input, bool _quiet)
{
	if (!_quiet)
		cout << "Input JSON: " << _input << endl;
	string outputString(solidity_compile(_input.c_str(), nullptr, nullptr));
	if (!_quiet)
		cout << "Output JSON: " << outputString << endl;

	// This should be safe given the above copies the output.
	solidity_reset();

	Json::Value output;
	if (!jsonParseStrict(outputString, output))
	{
		string msg{"Compiler produced invalid JSON output."};
		cout << msg << endl;
		throw std::runtime_error(std::move(msg));
	}
	if (output.isMember("errors"))
		for (auto const& error: output["errors"])
		{
			string invalid = findAnyOf(error["type"].asString(), vector<string>{
					"Exception",
					"InternalCompilerError"
			});
			if (!invalid.empty())
			{
				string msg = "Invalid error: \"" + error["type"].asString() + "\"";
				cout << msg << endl;
				throw std::runtime_error(std::move(msg));
			}
		}
}

void FuzzerUtil::testConstantOptimizer(string const& _input, bool _quiet)
{
	if (!_quiet)
		cout << "Testing constant optimizer" << endl;
	vector<u256> numbers;
	stringstream sin(_input);

	while (!sin.eof())
	{
		h256 data;
		sin.read(reinterpret_cast<char *>(data.data()), 32);
		numbers.push_back(u256(data));
	}
	if (!_quiet)
		cout << "Got " << numbers.size() << " inputs:" << endl;

	Assembly assembly;
	for (u256 const& n: numbers)
	{
		if (!_quiet)
			cout << n << endl;
		assembly.append(n);
	}
	for (bool isCreation: {false, true})
		for (unsigned runs: {1u, 2u, 3u, 20u, 40u, 100u, 200u, 400u, 1000u})
		{
			// Make a copy here so that each time we start with the original state.
			Assembly tmp = assembly;
			ConstantOptimisationMethod::optimiseConstants(
					isCreation,
					runs,
					langutil::EVMVersion{},
					tmp
			);
		}
}

void FuzzerUtil::testStandardCompiler(string const& _input, bool _quiet)
{
	if (!_quiet)
		cout << "Testing compiler via JSON interface." << endl;

	runCompiler(_input, _quiet);
}
