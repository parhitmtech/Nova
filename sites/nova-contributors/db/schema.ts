import { sqliteTable, text, integer, index } from 'drizzle-orm/sqlite-core';
export const applications = sqliteTable('applications', {
  id: text('id').primaryKey(), reference: text('reference').notNull().unique(), fingerprint: text('fingerprint').notNull(),
  name: text('name').notNull(), email: text('email').notNull(), level: text('experience_level').notNull(),
  interests: text('interests').notNull(), motivation: text('motivation').notNull(), experience: text('experience').notNull(),
  availability: text('availability').notNull(), timezone: text('timezone').notNull(), github: text('github_url').notNull(), resume: text('resume_url').notNull(),
  createdAt: integer('created_at').notNull(), consentAt: integer('consent_at').notNull(), ipHash: text('ip_hash').notNull(),
}, table => [index('applications_email_time').on(table.email, table.createdAt), index('applications_ip_time').on(table.ipHash, table.createdAt)]);
