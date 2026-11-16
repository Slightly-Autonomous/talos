# Supabase sync worker. Build from the repository root:
#   docker build -f connectors/supabase_sync/Dockerfile -t talos-supabase-sync .
FROM node:22-slim
RUN corepack enable
WORKDIR /app
COPY package.json pnpm-lock.yaml pnpm-workspace.yaml buf.yaml buf.gen.yaml ./
COPY contracts/proto contracts/proto
COPY packages packages
COPY connectors connectors
COPY web/package.json web/package.json
RUN pnpm install --frozen-lockfile && pnpm proto:gen
USER node
CMD ["pnpm", "--filter", "@talos/supabase-sync", "start"]
