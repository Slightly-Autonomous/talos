import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import {
  deviceHealth,
  previewRule,
  propertyOverview,
  requestNotification,
  requestSpeaker,
  searchActivity,
  type Hub,
} from "./tools.ts";

export function buildServer(hub: Hub): McpServer {
  const server = new McpServer({ name: "talos", version: "0.1.0" });
  server.tool(
    "property_overview",
    "Zones, devices, arm mode and current sensor states of the property.",
    async () => propertyOverview(hub),
  );
  server.tool(
    "search_activity",
    "Recorded sensor events, optionally filtered by event type and device.",
    {
      event_type: z.string().optional(),
      device_id: z.string().optional(),
      since_minutes: z.number().int().positive().max(10080).optional(),
      limit: z.number().int().positive().max(200).optional(),
    },
    async (args) => searchActivity(hub, args),
  );
  server.tool(
    "device_health",
    "Health status of every device, with recommended actions.",
    async () => deviceHealth(hub),
  );
  server.tool(
    "preview_rule",
    "Dry-run an automation rule (talos.v1.Rule JSON) against recent history. Never saves it.",
    { rule: z.record(z.unknown()), hours: z.number().positive().max(168).optional() },
    async ({ rule, hours }) => previewRule(hub, rule, hours),
  );
  server.tool(
    "request_speaker",
    "Ask the hub to speak a short message on a speaker. The hub's policy decides.",
    { speaker_device_id: z.string(), text: z.string().min(1).max(200) },
    async (args) => requestSpeaker(hub, args),
  );
  server.tool(
    "request_notification",
    "Ask the hub to notify the owner. The hub's policy decides.",
    { notifier_device_id: z.string(), text: z.string().min(1).max(500) },
    async (args) => requestNotification(hub, args),
  );
  return server;
}
