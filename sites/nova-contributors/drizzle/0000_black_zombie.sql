CREATE TABLE `applications` (
	`id` text PRIMARY KEY NOT NULL,
	`reference` text NOT NULL,
	`fingerprint` text NOT NULL,
	`name` text NOT NULL,
	`email` text NOT NULL,
	`experience_level` text NOT NULL,
	`interests` text NOT NULL,
	`motivation` text NOT NULL,
	`experience` text NOT NULL,
	`availability` text NOT NULL,
	`timezone` text NOT NULL,
	`github_url` text NOT NULL,
	`resume_url` text NOT NULL,
	`created_at` integer NOT NULL,
	`consent_at` integer NOT NULL,
	`ip_hash` text NOT NULL
);
--> statement-breakpoint
CREATE INDEX `applications_email_time` ON `applications` (`email`,`created_at`);--> statement-breakpoint
CREATE INDEX `applications_ip_time` ON `applications` (`ip_hash`,`created_at`);