# Nova contributor portal

A standalone, full-stack contributor application site for Nova. Any experience level is welcome. Contact links point to Parhitmtech’s GitHub profile; no public email or phone number has been invented.

## Features
- Interactive compiler walkthrough (explicitly illustrative; it does not execute code).
- Searchable contribution tracks, expandable details, participation guidance and FAQs.
- Accessible native-dialog application flow with review, validation, preserved in-page answers, real success references and recoverable errors.
- D1 persistence, idempotent submission references, bounded inputs, same-origin checks, honeypot and atomic email/IP submission limits.
- No public applicant listing or record-reading endpoint. Résumé/portfolio links are optional; uploads and automated emails are not included.

## Reviewing applications
The Site owner can inspect the `applications` table using the connected Sites database tools. Applicant records are never rendered on the public page. Site access is private by default; use the native Sites access controls when you decide to open the portal to visitors. Making the site public does not add a public record-reading API.

## Development
Use Node >=22.13 (or the repository’s `.nvmrc`). From the repository root:

```sh
cd sites/nova-contributors
npm run install:ci
npm run build
```

Apply migrations to the local database before testing applications:

```sh
for migration in drizzle/*.sql; do
  node --import ./scripts/sites-env.mjs ./node_modules/wrangler/bin/wrangler.js d1 execute DB --local --config dist/server/wrangler.json --persist-to .wrangler/state --file "$migration"
done
npm run dev -- --hostname 127.0.0.1 --port 4178
```

Apply each migration once per local database; the commands above initialize a fresh database. `npm run start -- --port 4178` previews the production build against the same local storage. Generate new schema migrations with `npm run db:generate`. Hosted Sites publication applies production migrations separately.

The existing Site is https://nova-compiler-contributors.syrupyseed2048.chatgpt.site and is private for owner review. The tracked project ID identifies that existing Site; it is not a credential. No secrets or local applicant data are included in this repository. Publishing on GitHub does not change the Site audience or deploy the Nova IDE.

`node --experimental-strip-types --test tests/application.test.mjs` verifies field validation. With the local preview and local migrations running, `node tests/integration.mjs` tests real persistence, idempotency, conflicts, origin/size/rate limits and private record access. These fixtures use reserved `example.test` addresses and are intentionally restricted to localhost. They do not contact applicants or create records in production.

## Verification
Production build, TypeScript checking, validation tests and real local D1 integration tests pass. Visual browser verification was unavailable because the computer-use screen-capture service failed. Responsive layouts and reduced-motion behavior are implemented but should be visually reviewed before broad release.
