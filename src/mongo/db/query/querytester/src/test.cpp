/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "test.h"

#include <fstream>
#include <thread>

#include "command_helpers.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/util/jparse_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/shell/shell_utils.h"

namespace queryTester {
namespace {
static const auto stringToModeOptionMap = std::map<std::string, ModeOption>{
    {"run", ModeOption::Run},
    {"compare", ModeOption::Compare},
    {"normalize", ModeOption::Normalize},
};

// If a line begins with a mode string, it is a test.
bool isTestLine(const std::string& line) {
    return line.starts_with(":");
}

mongo::BSONObj toBSONObj(const std::vector<mongo::BSONObj>& objs) {
    mongo::BSONObjBuilder bob;
    bob.append("res", objs.begin(), objs.end());
    return bob.obj();
}
}  // namespace

ModeOption stringToModeOption(const std::string& modeString) {
    if (auto itr = stringToModeOptionMap.find(modeString); itr != stringToModeOptionMap.end()) {
        return itr->second;
    } else {
        uasserted(9670422, "Only valid options for --mode are 'run', 'compare', and 'normalize'");
    }
}

std::vector<std::string> Test::normalize(const std::vector<mongo::BSONObj>& objs,
                                         const NormalizationOptsSet opts) {
    const auto numResults = objs.size();
    auto normalized = std::vector<std::string>(numResults);

    const auto numThreads =
        std::min(static_cast<size_t>(std::thread::hardware_concurrency()), numResults);
    std::vector<mongo::stdx::thread> threads;
    threads.reserve(numThreads);

    // Generate partitions and boundaries ahead of time.
    auto partitions = std::vector<size_t>{0};
    auto remainingResults = numResults;
    auto buckets = numThreads;

    for (; remainingResults > 0 && buckets > 0; --buckets) {
        auto step = remainingResults / buckets;
        partitions.push_back(partitions.back() + step);
        remainingResults -= step;
    }

    // Sanity checks.
    uassert(9670400,
            "Number of results processed by the partitions is not equal to the number of results.",
            partitions.back() == numResults);
    uassert(
        9670401, "Bucket count is not equal to thread count.", partitions.size() == numThreads + 1);

    for (auto sitr = partitions.begin(), eitr = partitions.begin() + 1; eitr != partitions.end();
         ++sitr, ++eitr) {
        threads.emplace_back([&, sitr, eitr]() {
            for (auto i = *sitr; i < *eitr; ++i) {
                // Either perform a direct results comparison or normalize each result in the result
                // set, which may involve sorting the fields and/or arrays within a result object,
                // as well as normalizing numerics to the same type.
                if (opts == NormalizationOpts::kResults) {
                    normalized[i] = objs[i].jsonString(mongo::ExtendedRelaxedV2_0_0, false, false);
                } else {
                    normalized[i] = mongo::shell_utils::normalizeBSONObj(objs[i], opts)
                                        .jsonString(mongo::ExtendedRelaxedV2_0_0, false, false);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Sort the top-level array of results.
    if (mongo::shell_utils::isSet(opts, NormalizationOpts::kSortResults)) {
        std::sort(normalized.begin(), normalized.end());
    }

    return normalized;
}

std::vector<mongo::BSONObj> Test::getAllResults(mongo::DBClientConnection* conn,
                                                const mongo::BSONObj& result) {
    const auto actualResult = commandHelpers::getResultsFromCommandResponse(result);
    const auto id = result.getField("cursor").embeddedObject()["id"].Long();

    auto actualObjs = std::vector<mongo::BSONObj>{};
    if (auto actualArr = actualResult.firstElement().Array(); !actualArr.empty()) {
        std::for_each(actualArr.begin(), actualArr.end(), [&actualObjs](const auto& elem) {
            actualObjs.push_back(elem.Obj().getOwned());
        });
    }

    // If cursor ID is 0, all results are in the first batch, and we don't need to call getMore.
    // Otherwise, call getMore() to retrieve the entire result set.
    if (id != 0) {
        mongo::DBClientCursor cursor(
            conn,
            mongo::NamespaceStringUtil::deserialize(
                boost::none,
                result.getField("cursor").embeddedObject().getStringField("ns"),
                mongo::SerializationContext::stateDefault()),
            id,
            false /*isExhaust*/);
        while (cursor.more()) {
            actualObjs.push_back(cursor.nextSafe().getOwned());
        }
    }

    return actualObjs;
}

void Test::runTestAndRecord(mongo::DBClientConnection* conn, const ModeOption mode) {
    // Populate _normalizedResult so that git diff operates on normalized result sets.
    _normalizedResult =
        normalize(mode == ModeOption::Normalize
                      ? _expectedResult
                      // Run test
                      : getAllResults(conn, commandHelpers::runCommand(conn, _db, _query)),
                  _testType);
}

Test Test::parseTest(std::fstream& fs, const ModeOption mode, const size_t testNum) {
    auto lineFromFile = std::string{};
    tassert(9670404,
            "Expected file to be open and ready for reading, but it wasn't",
            fs.is_open() && fs.good());
    auto preTestComments = fileHelpers::readLine(fs, lineFromFile);
    auto preQueryComments = std::vector<std::string>{};
    auto postQueryComments = std::vector<std::string>{};
    auto postTestComments = std::vector<std::string>{};

    const auto [testLine, testName] = [&fs, &preQueryComments, &lineFromFile, &testNum]()
        -> std::tuple<std::string, boost::optional<std::string>> {
        // First line can either be "# NAME" or test. Comments are skipped.
        if (isTestLine(lineFromFile)) {
            return {lineFromFile, boost::none};
        } else {
            auto testN = size_t{};
            auto testName = boost::optional<std::string>{};
            auto iss = std::istringstream{lineFromFile};
            iss >> testN;
            if (iss.eof()) {
                testName = boost::none;
            } else {
                auto testNameString = std::string{};
                iss >> testNameString;
                testName = testNameString;
            }
            uassert(9670451, "Non-test line should be either a '#' or a '# NAME'", iss.eof());
            uassert(9670440,
                    mongo::str::stream{} << "Expected test number (" << testNum
                                         << ") and read test number (" << testN
                                         << ") do not match.",
                    testN == testNum);
            // The first token should be a number. For now ignore and assume it lines
            // up with the number passed in. firstLine.front();
            preQueryComments = fileHelpers::readLine(fs, lineFromFile);
            uassert(9670423,
                    mongo::str::stream{} << "Expected test line for test #" << testNum
                                         << " but got " << lineFromFile,
                    isTestLine(lineFromFile));
            return {lineFromFile, testName};
        }
    }();

    auto expectedResult = std::vector<mongo::BSONObj>{};
    auto intraResultSetCommentLineCount = 0;
    if (mode == ModeOption::Normalize) {
        // Read in results set from file.
        for (postQueryComments = fileHelpers::readLine(fs, lineFromFile); lineFromFile != "";
             postTestComments = fileHelpers::readLine(fs, lineFromFile)) {
            intraResultSetCommentLineCount += postTestComments.size();
            if (lineFromFile.starts_with("[") && lineFromFile.ends_with("]")) {
                // Big performance hit, but this isn't going to be run in the
                // waterfall.
                for (auto&& ele : mongo::fromFuzzerJson("{ res : " + lineFromFile + "}")
                                      .firstElement()
                                      .Array()) {
                    expectedResult.push_back(ele.Obj().getOwned());
                }
            } else {
                auto sd = mongo::StringData{lineFromFile};
                {
                    auto endDocument = sd.rfind("}");
                    if (endDocument == std::string_view::npos) {
                        continue;
                    }
                    // Count offset of endDocument from the end, but still include the '}'
                    // character.
                    sd.remove_suffix(sd.size() - endDocument - 1);
                }
                {
                    auto startDocument = sd.find("{");
                    if (startDocument == std::string_view::npos) {
                        continue;
                    }
                    sd.remove_prefix(startDocument);
                }
                if (!sd.empty()) {
                    expectedResult.push_back(mongo::fromFuzzerJson(sd));
                }
            }
        }
        uassert(9670406,
                mongo::str::stream{} << "Expected results but found none for testNum " << testNum,
                !expectedResult.empty());

        if (intraResultSetCommentLineCount > 0) {
            std::cout << applyBrown() << "Warning: we ignored " << intraResultSetCommentLineCount
                      << " lines of intra-result set comments for test "
                      // Print test name or a backspace.
                      << testNum << " " << testName.value_or("\b") << "." << applyReset()
                      << std::endl;
        }

        return Test(testLine,
                    testNum,
                    testName,
                    std::move(preTestComments),
                    std::move(preQueryComments),
                    std::move(postQueryComments),
                    std::move(postTestComments),
                    std::move(expectedResult));
    } else {
        // There is a newline at the end of every test case.
        postQueryComments =
            fileHelpers::readAndAssertNewline(fs, "End of single test without results");
        return Test(testLine,
                    testNum,
                    testName,
                    std::move(preTestComments),
                    std::move(preQueryComments),
                    std::move(postQueryComments),
                    std::move(postTestComments));
    }
}

void Test::writeToStream(std::fstream& fs, const WriteOutOptions resultOpt) const {
    tassert(9670452,
            "Expected file to be open and ready for writing, but it wasn't",
            fs.is_open() && fs.good());
    for (const auto& comment : _comments.preTest) {
        fs << comment << std::endl;
    }
    fs << _testNum;
    if (_testName) {
        fs << " " << _testName.get();
    }
    fs << std::endl;
    for (const auto& comment : _comments.preQuery) {
        fs << comment << std::endl;
    }
    fs << _testLine << std::endl;
    for (const auto& comment : _comments.postQuery) {
        fs << comment << std::endl;
    }

    // This helps guard against WriteOutOptions that might get added but not handled.
    switch (resultOpt) {
        case WriteOutOptions::kOnelineResult: {
            // Print out just the array.
            fs << fileHelpers::LineResult<std::string>{_normalizedResult};
            break;
        }
        case WriteOutOptions::kResult: {
            // Print out each result in the result set on its own line.
            fs << fileHelpers::ArrayResult<std::string>{_normalizedResult};
            break;
        }
        default: {
            uasserted(9670436, "Writeout is not supported for that --out argument.");
        }
    }

    for (const auto& comment : _comments.postTest) {
        fs << comment << std::endl;
    }
}

void Test::parseTestQueryLine() {
    // First word is test type.
    auto endTestType = _testLine.find(' ');
    _testType = parseResultType(_testLine.substr(0, endTestType));

    // The rest of the string is the query.
    _query = mongo::fromFuzzerJson(_testLine.substr(endTestType + 1, _testLine.size()));
}
}  // namespace queryTester
