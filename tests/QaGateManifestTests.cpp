#include <catch2/catch_test_macros.hpp>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/interfaces/catch_interfaces_testcase.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// The gate manifest's rot guard.
//
// scripts/qa-gate.sh runs docs/qa-checklist.md's Part 1 as a set of Catch2
// test-spec filters declared in scripts/qa-gates.tsv. The runner already fails
// loudly when a filter matches nothing, because Catch2 exits non-zero on "no
// tests ran" - but it only finds out when somebody runs it, and by design that
// is on demand and on a release tag.
//
// The rot this closes is the other direction: a pull request renames a tag in
// tests/, every test still passes, CI is green, and the gate manifest silently
// stops covering it. That regression would surface on release day, in the one
// run nobody wants to be debugging. Asserting the manifest against the test
// registry makes it surface in the pull request that caused it instead.
//
// It deliberately checks tags rather than re-implementing Catch2's test-spec
// grammar. Every filter in the manifest is a comma-separated list of tags,
// which is the only form the manifest uses and the only form it should use;
// if a filter ever needs more than that, this test is the right place to find
// out that the manifest grew a feature.
namespace
{
    std::string trim (std::string value)
    {
        const auto notSpace = [] (unsigned char c) { return std::isspace (c) == 0; };

        value.erase (value.begin(), std::find_if (value.begin(), value.end(), notSpace));
        value.erase (std::find_if (value.rbegin(), value.rend(), notSpace).base(), value.end());

        return value;
    }

    struct ManifestRow
    {
        std::string id;
        std::string filter;
        std::string measure;
        std::string description;
    };

    std::vector<ManifestRow> readManifest (const std::string& path, bool& opened)
    {
        std::vector<ManifestRow> rows;
        std::ifstream stream (path);

        opened = stream.is_open();

        if (! opened)
            return rows;

        std::string line;

        while (std::getline (stream, line))
        {
            if (! line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty() || line[0] == '#')
                continue;

            std::vector<std::string> fields;
            std::string field;
            std::istringstream fieldStream (line);

            while (std::getline (fieldStream, field, '\t'))
                fields.push_back (trim (field));

            if (fields.size() < 4)
                continue;

            rows.push_back ({ fields[0], fields[1], fields[2], fields[3] });
        }

        return rows;
    }

    // "[a],[b]" -> { "[a]", "[b]" }. Anything that is not a bracketed tag is
    // returned verbatim so the caller can fail on it with a useful message
    // rather than silently ignoring it.
    std::vector<std::string> splitFilter (const std::string& filter)
    {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream stream (filter);

        while (std::getline (stream, part, ','))
        {
            part = trim (part);

            if (! part.empty())
                parts.push_back (part);
        }

        return parts;
    }

    std::set<std::string> registeredTags()
    {
        std::set<std::string> tags;

        for (const auto* info : Catch::getRegistryHub().getTestCaseRegistry().getAllInfos())
        {
            if (info == nullptr)
                continue;

            for (const auto& tag : info->tags)
            {
                // Tag::original carries the tag without its brackets.
                tags.insert ("[" + std::string (tag.original.data(), tag.original.size()) + "]");
            }
        }

        return tags;
    }

    std::string manifestPath()
    {
        return std::string (CRYPTA_REPO_ROOT) + "/scripts/qa-gates.tsv";
    }
}

//==============================================================================
TEST_CASE ("QA gate manifest: every declared filter still matches a real tag", "[qa-gate][meta]")
{
    bool opened = false;
    const auto rows = readManifest (manifestPath(), opened);

    INFO ("manifest: " << manifestPath());
    REQUIRE (opened);

    // If the manifest ever empties out, the gate would pass by running nothing.
    REQUIRE (rows.size() >= 8);

    const auto tags = registeredTags();
    REQUIRE (tags.size() > 20);

    for (const auto& row : rows)
    {
        INFO ("gate '" << row.id << "' declares filter '" << row.filter << "'");

        const auto parts = splitFilter (row.filter);
        CHECK_FALSE (parts.empty());

        for (const auto& part : parts)
        {
            INFO ("tag '" << part << "' is not carried by any registered test case - "
                  "either the tag was renamed and scripts/qa-gates.tsv was not, or the "
                  "manifest grew a test-spec form this guard does not understand");

            CHECK (part.size() > 2);
            CHECK (part.front() == '[');
            CHECK (part.back() == ']');
            CHECK (tags.count (part) == 1);
        }
    }
}

TEST_CASE ("QA gate manifest: gate ids are unique and usable on the command line", "[qa-gate][meta]")
{
    bool opened = false;
    const auto rows = readManifest (manifestPath(), opened);
    REQUIRE (opened);

    // qa-gate.sh's --gates option is a comma-separated list matched against
    // these ids, so an id containing a comma or whitespace would be
    // unselectable, and a duplicate would run twice and report twice.
    std::set<std::string> seen;

    for (const auto& row : rows)
    {
        INFO ("gate id: '" << row.id << "'");

        CHECK_FALSE (row.id.empty());
        CHECK (row.id.find (',') == std::string::npos);
        CHECK (row.id.find (' ') == std::string::npos);
        CHECK (seen.insert (row.id).second);

        // The description is what the report prints in the "Proves" column.
        // An empty one makes the report useless without making anything fail.
        CHECK_FALSE (row.description.empty());
    }
}
