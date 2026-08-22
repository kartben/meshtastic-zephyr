#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
"""Merge and scope the SARIF output of Zephyr's GCC SCA variant.

Building a Zephyr application with ``-DZEPHYR_SCA_VARIANT=gcc`` and
``-DGCC_SCA_OPTS=-fdiagnostics-format=sarif-file`` makes GCC write one SARIF
document per translation unit into the build directory.  A single ``twister``
run therefore leaves behind hundreds of them: mostly empty, and the non-empty
ones mostly pointing at upstream Zephyr or west module sources rather than at
code owned by this repository.

This script collapses those documents into the single SARIF run that GitHub
code scanning expects:

* only diagnostics whose primary location is a file inside the repository (and,
  optionally, below one of the ``--include`` prefixes) are kept;
* absolute paths are rewritten relative to the repository root, which is how
  code scanning matches a result to a file;
* the per-document ``artifacts`` arrays are dropped -- GCC embeds the full text
  of every source file in them, which alone blows past the SARIF upload size
  limit;
* code flow steps outside the repository are pruned, so no absolute path
  survives anywhere in the uploaded document;
* the CWE identifiers GCC reports through a SARIF taxonomy are re-expressed as
  ``external/cwe/cwe-NNN`` rule tags, which is the form code scanning renders;
* diagnostics reported identically by several builds of the same sources -- one
  per board and configuration a ``twister`` run covers -- are reported once.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse

# Diagnostics that GCC's static analyzer (-fanalyzer) emits; the -Werror= form
# is what a build that promotes warnings to errors reports.  Ordinary compiler
# warnings end up in the same SARIF documents; they are already surfaced by the
# Build workflow, so by default they are not reported a second time here.
DEFAULT_RULE_PREFIXES = ("-Wanalyzer-", "-Werror=analyzer-")

SCHEMA = ("https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/"
          "sarif-schema-2.1.0.json")
GCC_ANALYZER_DOCS = "https://gcc.gnu.org/onlinedocs/gcc/Static-Analyzer-Options.html"

# Keep the step summary readable; the full set is always in the SARIF artifact.
MAX_SUMMARY_ROWS = 50


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", type=Path,
                        help="SARIF files, or directories searched recursively for *.sarif")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd(),
                        help="repository root; results outside of it are dropped (default: cwd)")
    parser.add_argument("--include", action="append", default=[], metavar="PREFIX",
                        help="only keep results below this repo-relative path (repeatable)")
    parser.add_argument("--rule-prefix", action="append", default=None, metavar="PREFIX",
                        help=f"only keep results whose rule id starts with PREFIX (repeatable, "
                             f"default: {' '.join(DEFAULT_RULE_PREFIXES)}). Pass '' to keep every "
                             f"diagnostic.")
    parser.add_argument("--tool-name", default="GCC static analyzer",
                        help="tool name reported to code scanning (default: %(default)s)")
    parser.add_argument("--output", type=Path, required=True, help="merged SARIF file to write")
    parser.add_argument("--summary", type=Path,
                        help="Markdown file to append a human readable summary to")
    parser.add_argument("--fail-on-findings", action="store_true",
                        help="exit non-zero when at least one finding was kept")
    return parser.parse_args(argv)


def iter_sarif_files(inputs):
    for path in inputs:
        if path.is_dir():
            yield from sorted(path.rglob("*.sarif"))
        elif path.exists():
            yield path


def uri_to_path(uri, base):
    """Turn a SARIF artifact URI into an absolute filesystem path."""
    if uri.startswith("file:"):
        uri = unquote(urlparse(uri).path)
    path = Path(uri)
    if not path.is_absolute():
        # GCC runs from the build directory, which is where it drops the SARIF.
        path = base / path
    return Path(os.path.normpath(path))


class Scope:
    """Decides which artifact URIs belong to the repository."""

    def __init__(self, repo_root, includes):
        self.repo_root = repo_root.resolve()
        self.includes = [Path(inc) for inc in includes]

    def relative(self, uri, base):
        """Return the repo-relative POSIX path of `uri`, or None if out of scope."""
        try:
            rel = uri_to_path(uri, base).relative_to(self.repo_root)
        except ValueError:
            return None
        if self.includes and not any(rel == inc or inc in rel.parents for inc in self.includes):
            return None
        return rel.as_posix()


def rewrite_locations(node, scope, base):
    """Rewrite in-scope artifact URIs in place; report whether all were in scope."""
    complete = True
    if isinstance(node, dict):
        location = node.get("artifactLocation")
        if isinstance(location, dict) and "uri" in location:
            rel = scope.relative(location["uri"], base)
            if rel is None:
                complete = False
            else:
                location["uri"] = rel
                location.pop("uriBaseId", None)
        for value in node.values():
            complete &= rewrite_locations(value, scope, base)
    elif isinstance(node, list):
        for value in node:
            complete &= rewrite_locations(value, scope, base)
    return complete


def prune_code_flows(result, scope, base):
    """Drop code flow steps that point outside the repository."""
    flows = []
    for flow in result.get("codeFlows", []):
        threads = []
        for thread in flow.get("threadFlows", []):
            steps = [step for step in thread.get("locations", [])
                     if rewrite_locations(step, scope, base)]
            if steps:
                threads.append({**thread, "locations": steps})
        if threads:
            flows.append({**flow, "threadFlows": threads})
    if flows:
        result["codeFlows"] = flows
    else:
        result.pop("codeFlows", None)


def result_rule_id(result, rules):
    rule_id = result.get("ruleId")
    if rule_id is None:
        index = result.get("ruleIndex")
        if isinstance(index, int) and 0 <= index < len(rules):
            rule_id = rules[index].get("id")
    return rule_id


def primary_location(result):
    locations = result.get("locations") or []
    if not locations:
        return None
    return locations[0].get("physicalLocation")


def collect(args, scope, rule_prefixes):
    """Walk every input document and return the results worth reporting."""
    findings = []
    seen = set()
    rules = {}
    tags = {}
    versions = []
    documents = 0

    for sarif_file in iter_sarif_files(args.inputs):
        try:
            document = json.loads(sarif_file.read_text())
        except (OSError, ValueError) as err:
            print(f"warning: skipping {sarif_file}: {err}", file=sys.stderr)
            continue

        documents += 1
        base = sarif_file.parent.resolve()

        for run in document.get("runs", []):
            driver = run.get("tool", {}).get("driver", {})
            run_rules = driver.get("rules") or []
            version = driver.get("version")
            if version and version not in versions:
                versions.append(version)

            taxa_by_id = {taxon.get("id"): taxonomy.get("name")
                          for taxonomy in run.get("taxonomies", [])
                          for taxon in taxonomy.get("taxa", [])}

            for result in run.get("results", []):
                rule_id = result_rule_id(result, run_rules)
                if rule_id is None:
                    continue
                if rule_prefixes and not any(rule_id.startswith(p) for p in rule_prefixes):
                    continue

                physical = primary_location(result)
                if physical is None:
                    continue
                rel = scope.relative(physical.get("artifactLocation", {}).get("uri", ""), base)
                if rel is None:
                    continue

                # Work on a copy: every location the result still points at has
                # to come out repo relative.
                result = json.loads(json.dumps(result))
                prune_code_flows(result, scope, base)
                result["relatedLocations"] = [
                    related for related in result.get("relatedLocations", [])
                    if rewrite_locations(related, scope, base)
                ]
                if not result["relatedLocations"]:
                    del result["relatedLocations"]
                rewrite_locations(result.get("locations", []), scope, base)

                # Code scanning renders CWE identifiers from rule tags, so
                # translate GCC's taxonomy references and drop them.
                for reference in result.pop("taxa", []):
                    if taxa_by_id.get(reference.get("id")) == "CWE":
                        tags.setdefault(rule_id, set()).add(
                            f"external/cwe/cwe-{reference['id']}")

                result.pop("ruleIndex", None)
                result["ruleId"] = rule_id

                # The same sources are built once per board and configuration.
                region = physical.get("region", {})
                key = (rel, region.get("startLine"), region.get("startColumn"), rule_id,
                       result.get("message", {}).get("text"))
                if key in seen:
                    continue
                seen.add(key)
                findings.append((rel, result))

                for rule in run_rules:
                    if rule.get("id") == rule_id:
                        rules.setdefault(rule_id, rule)

    return findings, rules, tags, versions, documents


def build_document(args, findings, rules, tags, versions):
    rule_ids = list(rules)
    for rule_id, cwes in tags.items():
        properties = rules[rule_id].setdefault("properties", {})
        properties["tags"] = sorted(set(properties.get("tags", [])) | cwes)

    results = []
    for _, result in findings:
        result["ruleIndex"] = rule_ids.index(result["ruleId"])
        results.append(result)

    driver = {
        "name": args.tool_name,
        "informationUri": GCC_ANALYZER_DOCS,
        "rules": [rules[rule_id] for rule_id in rule_ids],
    }
    if versions:
        driver["version"] = ", ".join(versions)

    return {"$schema": SCHEMA, "version": "2.1.0",
            "runs": [{"tool": {"driver": driver}, "results": results}]}


def write_summary(args, findings, documents):
    lines = [f"## {args.tool_name}", ""]
    if not findings:
        lines.append(f"No findings in repository sources "
                     f"({documents} translation units analysed).")
    else:
        lines += [
            f"**{len(findings)}** finding(s) in repository sources "
            f"({documents} translation units analysed).",
            "",
            "| File | Line | Rule | Message |",
            "| --- | ---: | --- | --- |",
        ]
        for rel, result in findings[:MAX_SUMMARY_ROWS]:
            region = primary_location(result).get("region", {})
            message = result.get("message", {}).get("text", "").replace("|", "\\|")
            lines.append(f"| `{rel}` | {region.get('startLine', '')} | "
                         f"`{result['ruleId']}` | {message} |")
        if len(findings) > MAX_SUMMARY_ROWS:
            lines.append(f"| … | | | {len(findings) - MAX_SUMMARY_ROWS} more, see the "
                         f"uploaded SARIF |")
    lines.append("")

    text = "\n".join(lines)
    print(text)
    if args.summary:
        with args.summary.open("a") as summary:
            summary.write(text)


def main(argv=None):
    args = parse_args(argv)
    rule_prefixes = DEFAULT_RULE_PREFIXES if args.rule_prefix is None else \
        [prefix for prefix in args.rule_prefix if prefix]
    scope = Scope(args.repo_root, args.include)

    findings, rules, tags, versions, documents = collect(args, scope, rule_prefixes)
    if not documents:
        print("error: no SARIF files found, did the build really run with "
              "-DGCC_SCA_OPTS=-fdiagnostics-format=sarif-file?", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(build_document(args, findings, rules, tags, versions)))
    write_summary(args, findings, documents)

    return 1 if findings and args.fail_on_findings else 0


if __name__ == "__main__":
    sys.exit(main())
