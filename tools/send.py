#!/usr/bin/env python3
"""Send an example screen to the device over HTTP MCP (stdlib only).

    python3 tools/send.py http://192.168.1.42:9000/mcp examples/metrics.json
    python3 tools/send.py http://192.168.1.42:9000/mcp --tool notify '{"text":"你好"}'
    python3 tools/send.py http://192.168.1.42:9000/mcp --list

A screen file's "type" picks the tool (text -> show_text, ...); a file without
one is sent to notify.
"""
import argparse
import json
import sys
import urllib.request

TOOLS = {
    "text": "show_text",
    "notice": "show_notice",
    "list": "show_list",
    "metrics": "show_metrics",
    "layout": "show_layout",
    "html": "show_html",
    "slides": "show_slides",
}


# The device is on the LAN; an HTTP(S)_PROXY from the environment would only
# swallow the request.
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def rpc(url, method, params=None, rid=1):
    body = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        body["params"] = params
    headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    req = urllib.request.Request(url, json.dumps(body, ensure_ascii=False).encode("utf-8"), headers)
    with OPENER.open(req, timeout=30) as resp:
        return json.loads(resp.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("screen", nargs="?", help="JSON/HTML file, or inline JSON with --tool")
    ap.add_argument("--tool", help="call this tool with the given arguments")
    ap.add_argument("--list", action="store_true", help="list the device's tools")
    args = ap.parse_intermixed_args()

    init = rpc(args.url, "initialize", {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "send.py", "version": "1.0"},
    })
    print("server:", init.get("result", {}).get("serverInfo"))

    if args.list:
        for t in rpc(args.url, "tools/list", {}, 2)["result"]["tools"]:
            print(f"  {t['name']:18s} {t['description'][:70]}")
        return
    if not args.screen:
        sys.exit("give a screen file or --list")

    if args.tool:
        tool = args.tool
        text = args.screen if args.screen.lstrip().startswith("{") else open(args.screen).read()
        params = json.loads(text)
    elif args.screen.endswith(".html"):
        with open(args.screen, encoding="utf-8") as source:
            params = {"html": source.read(), "persist": False}
        tool = "show_html"
    else:
        params = json.load(open(args.screen))
        tool = TOOLS.get(params.pop("type", None), "notify")

    res = rpc(args.url, "tools/call", {"name": tool, "arguments": params}, 2)
    result = res.get("result", res)
    print(tool, "->", json.dumps(result.get("structuredContent", result), ensure_ascii=False))


if __name__ == "__main__":
    main()
