import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Nova — Build what comes next",
  description: "An open invitation to build Nova. Explore compilers, find your contribution track, and apply at any experience level.",
  icons: {
    icon: "/favicon.svg",
    shortcut: "/favicon.svg",
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className="antialiased">{children}</body>
    </html>
  );
}
