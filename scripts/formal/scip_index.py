#!/usr/bin/env python3
"""Minimal SCIP index reader for the FTLR-0 formal-impact pilot (#299).

Vendored, dependency-free protobuf wire-format reader for the subset of the
SCIP schema (schema/scip.proto, sourcegraph/scip; validated against
scip-clang v0.4.0 output) that formal_impact.py needs:

    Index.metadata (1), Index.documents (2)
    Document.relative_path (1), Document.occurrences (2), Document.symbols (3)
    SymbolInformation.symbol (1), SymbolInformation.relationships (4),
    SymbolInformation.kind (5)
    Relationship.symbol (1), Relationship.is_reference (2),
    Relationship.is_implementation (3)
    Occurrence.range (1, packed varint), Occurrence.symbol (2),
    Occurrence.symbol_roles (3)

Any unknown field is skipped by wire type, so a newer schema (e.g. the
typed_range oneof fields 8-11) still decodes losslessly for the fields above.
Field numbers were verified against a real scip-clang v0.4.0 index of this
repository, not assumed from the schema text alone (S9 guards decode errors).

Observed scip-clang v0.4.0 C++ symbol format (scheme `cxx`, empty package
fields `. . $`):

    cxx . . $ sluice/async/Scheduler#signal_wake_locked(49f6e7a06ebc5aa8).
              ^^^^^ namespace components end with '/'
                    ^^^^^^^^^ class components end with '#'
                              ^^^^^^^^^^^^^^^^^^^^^^^^^^^ method component:
                              name + '(' signature-hash ')' + '.'

The method signature hash is opaque and overload-disambiguating: anchors are
resolved by NAME SEGMENTS (never by hash), so template/overload families
resolve as one symbol family and the identity survives cross-file moves
(T7). Occurrences carry no enclosing_range from scip-clang v0.4.0 (field 7
absent), so reference attribution to an enclosing function uses the
nearest-preceding function-kind definition in the same document (see
build_graph).
"""
from __future__ import annotations

import bisect
from dataclasses import dataclass, field

# --- protobuf wire format -------------------------------------------------


def read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def iter_fields(buf: bytes):
    """Yield (field_number, wire_type, value) over a protobuf message."""
    pos = 0
    end = len(buf)
    while pos < end:
        key, pos = read_varint(buf, pos)
        field_number, wire_type = key >> 3, key & 7
        if field_number == 0:
            raise ValueError("invalid field number 0")
        if wire_type == 0:
            value, pos = read_varint(buf, pos)
        elif wire_type == 2:
            length, pos = read_varint(buf, pos)
            if pos + length > end:
                raise ValueError("truncated length-delimited field")
            value = buf[pos : pos + length]
            pos += length
        elif wire_type == 5:
            value, pos = buf[pos : pos + 4], pos + 4
        elif wire_type == 1:
            value, pos = buf[pos : pos + 8], pos + 8
        else:
            raise ValueError(f"unsupported wire type {wire_type}")
        yield field_number, wire_type, value


def packed_ints(buf: bytes) -> list[int]:
    """Decode a packed repeated int32 payload (handles both packed varints
    under wire type 2 and legacy unpacked values)."""
    out = []
    pos = 0
    while pos < len(buf):
        value, pos = read_varint(buf, pos)
        out.append(value)
    return out


# --- SCIP message parsing -------------------------------------------------


@dataclass
class Relationship:
    symbol: str
    is_reference: bool = False
    is_implementation: bool = False


@dataclass
class SymbolInfo:
    symbol: str
    kind: int = 0
    relationships: list = field(default_factory=list)


@dataclass
class Occurrence:
    range: list  # [start_line, start_col, end_line, end_col] normalized to 4
    symbol: str
    symbol_roles: int = 0
    raw_range_len: int = 0

    @property
    def is_definition(self) -> bool:
        # SymbolRole.Definition = 1 (bitfield LSB).
        return bool(self.symbol_roles & 1)

    @property
    def sort_key(self) -> tuple[int, int]:
        return (self.range[0], self.range[1])


@dataclass
class Document:
    relative_path: str
    occurrences: list  # [Occurrence]
    symbols: list  # [SymbolInfo]


def parse_occurrence(buf: bytes) -> Occurrence:
    rng: list[int] = []
    sym = ""
    roles = 0
    for field_number, wire_type, value in iter_fields(buf):
        if field_number == 1 and wire_type == 2:
            rng = packed_ints(value)
        elif field_number == 1 and wire_type == 0:
            rng.append(value)
        elif field_number == 2 and wire_type == 2:
            sym = value.decode("utf-8")
        elif field_number == 3 and wire_type == 0:
            roles = value
    if not rng or len(rng) < 3 or len(rng) > 4:
        raise ValueError(f"malformed occurrence range: {rng!r}")
    raw_len = len(rng)
    if len(rng) == 3:
        # [startLine, startCharacter, endCharacter] — single-line.
        rng = [rng[0], rng[1], rng[0], rng[2]]
    return Occurrence(range=rng, symbol=sym, symbol_roles=roles, raw_range_len=raw_len)


def parse_symbol_information(buf: bytes) -> SymbolInfo:
    sym = ""
    kind = 0
    rels: list[Relationship] = []
    for field_number, wire_type, value in iter_fields(buf):
        if field_number == 1 and wire_type == 2:
            sym = value.decode("utf-8")
        elif field_number == 4 and wire_type == 2:
            rel_sym = ""
            is_ref = False
            is_impl = False
            for f2, w2, v2 in iter_fields(value):
                if f2 == 1 and w2 == 2:
                    rel_sym = v2.decode("utf-8")
                elif f2 == 2 and w2 == 0:
                    is_ref = bool(v2)
                elif f2 == 3 and w2 == 0:
                    is_impl = bool(v2)
            rels.append(Relationship(symbol=rel_sym, is_reference=is_ref, is_implementation=is_impl))
        elif field_number == 5 and wire_type == 0:
            kind = value
    if not sym:
        raise ValueError("SymbolInformation without symbol")
    return SymbolInfo(symbol=sym, kind=kind, relationships=rels)


def parse_document(buf: bytes) -> Document:
    path = ""
    occurrences: list[Occurrence] = []
    symbols: list[SymbolInfo] = []
    for field_number, wire_type, value in iter_fields(buf):
        if field_number == 1 and wire_type == 2:
            path = value.decode("utf-8")
        elif field_number == 2 and wire_type == 2:
            occurrences.append(parse_occurrence(value))
        elif field_number == 3 and wire_type == 2:
            symbols.append(parse_symbol_information(value))
    if not path:
        raise ValueError("Document without relative_path")
    return Document(relative_path=path, occurrences=occurrences, symbols=symbols)


@dataclass
class IndexMeta:
    tool_name: str = ""
    tool_version: str = ""
    project_root: str = ""


def parse_index(data: bytes) -> tuple[IndexMeta, list[Document]]:
    meta = IndexMeta()
    documents: list[Document] = []
    for field_number, wire_type, value in iter_fields(data):
        if field_number == 1 and wire_type == 2:
            for f2, w2, v2 in iter_fields(value):
                if f2 == 2 and w2 == 2:
                    for f3, w3, v3 in iter_fields(v2):
                        if f3 == 1 and w3 == 2:
                            meta.tool_name = v3.decode("utf-8")
                        elif f3 == 2 and w3 == 2:
                            meta.tool_version = v3.decode("utf-8")
                elif f2 == 3 and w2 == 2:
                    meta.project_root = v2.decode("utf-8")
        elif field_number == 2 and wire_type == 2:
            documents.append(parse_document(value))
    if not documents:
        raise ValueError("SCIP index contains no documents")
    return meta, documents


# --- symbol naming --------------------------------------------------------


def symbol_segments(scip_symbol: str) -> list[str] | None:
    """Split a scip-clang C++ symbol into human-readable name segments.

    Returns None for symbols without a usable name path (file symbols like
    "cxx . . $ `<file>/...`/" and local symbols starting with '$').
    """
    parts = split_symbol(scip_symbol)
    if parts is None:
        return None
    return parts[0]


def is_namespace_symbol(scip_symbol: str) -> bool:
    """True for namespace/package descriptors (terminal component ends with
    '/'). Namespace definition nodes are reference hubs — every symbol whose
    body spells out the qualifier references them — so formal_impact.py
    excludes them from changed-symbol attribution and traversal expansion
    (the T6 specimen demonstrated the false-positive pump)."""
    parts = split_symbol(scip_symbol)
    if parts is None:
        return False
    return parts[1]


def split_symbol(scip_symbol: str) -> tuple[list[str], bool] | None:
    """Returns (segments, is_namespace) or None for unusable symbols."""
    # Symbol grammar: "<scheme> <package-manager> <package-name> <version> <descriptors>"
    parts = scip_symbol.split(" ", 4)
    if len(parts) != 5:
        return None
    scheme, _pm, _pn, _ver, descriptors = parts
    if scheme not in ("cxx", "c++"):
        return None
    if descriptors.startswith("`<file>") or descriptors.startswith("$"):
        return None
    is_namespace = descriptors.endswith("/")
    # Descriptor components are terminated by '/', '#', or '.'; a method
    # component additionally carries a '(<signature-hash>)' before its
    # terminator. Iterate with a small scanner rather than regex so
    # backtick-quoted operator names survive.
    segments: list[str] = []
    i = 0
    n = len(descriptors)
    current: list[str] = []
    while i < n:
        ch = descriptors[i]
        if ch == "`":
            # quoted identifier (e.g. `operator()`)
            j = descriptors.find("`", i + 1)
            if j < 0:
                return None
            current.append(descriptors[i : j + 1])
            i = j + 1
            continue
        if ch == "(":
            # signature-hash / parameter-list suffix: skip to matching ')'
            depth = 0
            j = i
            while j < n:
                if descriptors[j] == "(":
                    depth += 1
                elif descriptors[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if j >= n:
                return None
            i = j + 1
            continue
        if ch in "/#.":
            name = "".join(current)
            if name:
                segments.append(name)
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    # Trailing segment without terminator (rare; tolerate).
    tail = "".join(current)
    if tail:
        segments.append(tail)
    if not segments:
        return None
    return segments, is_namespace


def display_name(scip_symbol: str) -> str | None:
    segments = symbol_segments(scip_symbol)
    return "::".join(segments) if segments else None


def matches_anchor(segments: list[str], anchor_segments: list[str]) -> bool:
    """True when anchor_segments are a suffix of segments."""
    if len(anchor_segments) > len(segments):
        return False
    return segments[-len(anchor_segments) :] == anchor_segments


# --- graph construction ----------------------------------------------------


@dataclass
class GraphNode:
    symbol: str
    display: str
    segments: list[str]
    kind: int
    namespace_hub: bool = False
    def_files: set = field(default_factory=set)
    def_range: list | None = None
    refs: set = field(default_factory=set)


def build_graph(documents: list[Document]) -> dict:
    """Build a deterministic symbol-level reference graph from SCIP documents.

    Nodes are repo-defined symbols (symbols with at least one definition
    occurrence). Edges are "references": a definition node in a document
    references every symbol occurrence attributed inside its span. Because
    scip-clang v0.4.0 emits no enclosing ranges, attribution uses the
    nearest-preceding function-kind definition occurrence in the same
    document (deterministic; header field-initializer attribution is a
    documented approximation).
    """
    nodes: dict[str, GraphNode] = {}

    # Pass 1: definition sites per document. scip-clang v0.4.0 emits no
    # SymbolInformation.kind (field 5 absent; verified empirically), so
    # attribution anchors are all NAME-BEARING definition occurrences (local
    # `local N` symbols and file symbols carry no name path and are skipped).
    for doc in documents:
        kind_by_symbol = {si.symbol: si.kind for si in doc.symbols}
        defs = [occ for occ in doc.occurrences if occ.is_definition and occ.symbol]
        defs.sort(key=lambda o: o.sort_key)
        named_defs: list[tuple[tuple[int, int], str]] = []
        for occ in defs:
            segs = symbol_segments(occ.symbol)
            if segs is None:
                continue
            named_defs.append((occ.sort_key, occ.symbol))
            node = nodes.get(occ.symbol)
            if node is None:
                node = GraphNode(
                    symbol=occ.symbol,
                    display="::".join(segs),
                    segments=segs,
                    kind=kind_by_symbol.get(occ.symbol, 0),
                    namespace_hub=is_namespace_symbol(occ.symbol),
                )
                nodes[occ.symbol] = node
            node.def_files.add(doc.relative_path)
            if node.def_range is None:
                node.def_range = list(occ.range)

        # Nearest-preceding named definition attribution.
        attr_positions = [pos for pos, _sym in named_defs]

        # Pass 2: attribute reference occurrences to the nearest-preceding
        # named definition in this document.
        for occ in doc.occurrences:
            if not occ.symbol or occ.is_definition:
                continue
            idx = bisect.bisect_right(attr_positions, occ.sort_key) - 1
            if idx < 0:
                continue  # file-scope reference: no enclosing named node
            caller = named_defs[idx][1]
            callee_node = nodes.get(caller)
            if callee_node is None:
                # Caller defined only in another document variant; register
                # a lightweight node so edges are not lost.
                segs = symbol_segments(caller)
                if segs is None:
                    continue
                callee_node = GraphNode(
                    symbol=caller,
                    display="::".join(segs),
                    segments=segs,
                    kind=kind_by_symbol.get(caller, 0),
                    namespace_hub=is_namespace_symbol(caller),
                )
                nodes[caller] = callee_node
            callee_node.refs.add(occ.symbol)

    # Keep only reference edges whose target is a repo-defined node; count
    # dropped external references (std:: etc.) for stats.
    dropped_external = 0
    reverse: dict[str, list[str]] = {}
    for node in nodes.values():
        kept = set()
        for ref in node.refs:
            if ref in nodes:
                kept.add(ref)
                reverse.setdefault(ref, []).append(node.symbol)
            else:
                dropped_external += 1
        node.refs = kept

    documents_map = {
        doc.relative_path: sorted(
            {occ.symbol for occ in doc.occurrences if occ.is_definition and occ.symbol in nodes}
        )
        for doc in documents
    }

    # Per-document definition positions (named defs, sorted) — the same
    # nearest-preceding-definition rule the diff hunk attribution in
    # formal_impact.py uses, so body-only edits attribute to their enclosing
    # definition consistently with reference attribution.
    def_positions: dict[str, list] = {}
    for doc in documents:
        entries = []
        for occ in doc.occurrences:
            if occ.is_definition and occ.symbol in nodes:
                entries.append((occ.range[0], occ.range[1], occ.symbol))
        if entries:
            def_positions[doc.relative_path] = sorted(entries)

    return {
        "nodes": {sym: node for sym, node in sorted(nodes.items())},
        "reverse": {sym: sorted(set(syms)) for sym, syms in sorted(reverse.items())},
        "documents": {path: syms for path, syms in sorted(documents_map.items())},
        "def_positions": {path: pos for path, pos in sorted(def_positions.items())},
        "stats": {
            "documents": len(documents),
            "symbols": len(nodes),
            "reference_edges": sum(len(n.refs) for n in nodes.values()),
            "dropped_external_refs": dropped_external,
        },
    }


# --- serialization helpers -------------------------------------------------


def graph_to_json(graph: dict) -> dict:
    nodes_json = {}
    for sym, node in sorted(graph["nodes"].items()):
        nodes_json[sym] = {
            "display": node.display,
            "segments": node.segments,
            "kind": node.kind,
            "ns": node.namespace_hub,
            "def_files": sorted(node.def_files),
            "def_range": node.def_range,
            "refs": sorted(node.refs),
        }
    return {
        "nodes": nodes_json,
        "reverse": graph["reverse"],
        "documents": graph["documents"],
        "def_positions": graph.get("def_positions", {}),
        "stats": graph["stats"],
    }


def graph_from_json(data: dict) -> dict:
    nodes = {}
    for sym, nj in data["nodes"].items():
        nodes[sym] = GraphNode(
            symbol=sym,
            display=nj["display"],
            segments=nj["segments"],
            kind=nj["kind"],
            namespace_hub=nj.get("ns", False),
            def_files=set(nj["def_files"]),
            def_range=nj["def_range"],
            refs=set(nj["refs"]),
        )
    return {
        "nodes": nodes,
        "reverse": data["reverse"],
        "documents": data["documents"],
        "stats": data["stats"],
    }
