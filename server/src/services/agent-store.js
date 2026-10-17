'use strict';
// NovaBot conversation history and per-user memory (DynamoDB), and the personalised system prompt.
const { AGENT_HISTORY_TABLE, AGENT_MEMORY_TABLE } = require('../config');
const { dynamo } = require('./aws');
const { PutCommand, GetCommand, DeleteCommand, QueryCommand } = require('@aws-sdk/lib-dynamodb');

// ── Conversation history helpers ──────────────────────────────────────────────

async function saveMessage(userId, role, content) {
  const timestamp = Date.now();
  const expiresAt = Math.floor(timestamp / 1000) + 30 * 24 * 60 * 60; // 30-day TTL
  await dynamo.send(new PutCommand({
    TableName: AGENT_HISTORY_TABLE,
    Item: { userId, timestamp, role, content: content.slice(0, 10000), expiresAt },
  }));
}

async function loadHistory(userId, limit = 20) {
  const result = await dynamo.send(new QueryCommand({
    TableName: AGENT_HISTORY_TABLE,
    KeyConditionExpression: 'userId = :uid',
    ExpressionAttributeValues: { ':uid': userId },
    ScanIndexForward: false,
    Limit: limit,
  }));
  return (result.Items || []).reverse().map(item => ({
    role: item.role, content: item.content, timestamp: item.timestamp,
  }));
}

async function clearHistory(userId) {
  const history = await loadHistory(userId, 100);
  for (const msg of history) {
    await dynamo.send(new DeleteCommand({
      TableName: AGENT_HISTORY_TABLE,
      Key: { userId, timestamp: msg.timestamp },
    }));
  }
}

// ── User memory helpers ───────────────────────────────────────────────────────

async function loadUserMemory(userId) {
  try {
    const result = await dynamo.send(new GetCommand({
      TableName: AGENT_MEMORY_TABLE,
      Key: { userId },
    }));
    return result.Item || { userId, preferences: {}, workingContext: {}, facts: [] };
  } catch {
    return { userId, preferences: {}, workingContext: {}, facts: [] };
  }
}

async function saveUserMemory(userId, memory) {
  await dynamo.send(new PutCommand({
    TableName: AGENT_MEMORY_TABLE,
    Item: { ...memory, userId, updatedAt: new Date().toISOString() },
  }));
}

async function updateMemoryFromConversation(userId, userMessage, assistantResponse) {
  const memory = await loadUserMemory(userId);
  const lowerMsg = userMessage.toLowerCase();

  if (lowerMsg.includes('recursive') || lowerMsg.includes('recursion'))
    memory.preferences.solutionStyle = 'recursive';
  if (lowerMsg.includes('iterative') || lowerMsg.includes('loop') && !lowerMsg.includes('recursion'))
    memory.preferences.solutionStyle = 'iterative';

  if (lowerMsg.includes('explain') || lowerMsg.includes("don't understand") ||
      lowerMsg.includes('what is') || lowerMsg.includes('how does'))
    memory.preferences.experienceLevel = 'beginner';
  if (lowerMsg.includes('optimize') || lowerMsg.includes('performance') ||
      lowerMsg.includes('complexity'))
    memory.preferences.experienceLevel = 'advanced';

  if (lowerMsg.includes('neural network') || lowerMsg.includes(' ml ') || lowerMsg.includes('tensor'))
    memory.workingContext.currentProject = 'machine_learning';
  if (lowerMsg.includes('sort') || lowerMsg.includes('search') || lowerMsg.includes('algorithm'))
    memory.workingContext.currentProject = 'algorithms';

  if (lowerMsg.includes('error') || lowerMsg.includes('not working') || lowerMsg.includes('broken')) {
    memory.workingContext.lastIssue   = userMessage.slice(0, 200);
    memory.workingContext.lastIssueAt = new Date().toISOString();
  }

  memory.facts = [`User asked about: ${userMessage.slice(0, 100)}`, ...(memory.facts || [])].slice(0, 10);
  await saveUserMemory(userId, memory);
  return memory;
}

// ── Personalized system prompt ────────────────────────────────────────────────

function buildPersonalizedSystemPrompt(basePrompt, memory, userEmail) {
  let section = `\n\n## User Context\nUser: ${userEmail}\n`;

  if (memory.preferences.experienceLevel) {
    section += `Experience level: ${memory.preferences.experienceLevel}\n`;
    if (memory.preferences.experienceLevel === 'beginner')
      section += `→ Explain concepts clearly with examples. Avoid jargon.\n`;
    else if (memory.preferences.experienceLevel === 'advanced')
      section += `→ Be concise. Focus on performance and best practices.\n`;
  }

  if (memory.preferences.solutionStyle)
    section += `Preferred style: ${memory.preferences.solutionStyle} — default to this unless asked otherwise.\n`;

  const projectNames = { machine_learning: 'ML/tensor operations', algorithms: 'algorithms and data structures' };
  if (memory.workingContext.currentProject)
    section += `Currently working on: ${projectNames[memory.workingContext.currentProject] || memory.workingContext.currentProject}\n`;

  if (memory.workingContext.lastIssue)
    section += `Last issue: ${memory.workingContext.lastIssue}\n`;

  if (memory.facts?.length > 0) {
    section += `\nRecent topics:\n`;
    memory.facts.slice(0, 5).forEach(f => { section += `- ${f}\n`; });
  }

  return basePrompt + section;
}

module.exports = {
  saveMessage, loadHistory, clearHistory, loadUserMemory, saveUserMemory,
  updateMemoryFromConversation, buildPersonalizedSystemPrompt,
};
