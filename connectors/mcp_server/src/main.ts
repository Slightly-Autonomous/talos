// Talos MCP server over stdio. Configure with env vars only:
//   HUB_URL, HUB_TOKEN (an AGENT principal token: policy-capped)
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { HubClient } from "@talos/contracts/client";
import { buildServer } from "./server.ts";

const url = process.env.HUB_URL;
const token = process.env.HUB_TOKEN;
if (!url || !token) {
  console.error("HUB_URL and HUB_TOKEN (agent principal) are required");
  process.exit(2);
}
await buildServer(new HubClient({ baseUrl: url, token })).connect(new StdioServerTransport());
