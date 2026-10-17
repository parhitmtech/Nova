'use strict';
// NovaBot: POST /agent/chat (SSE), GET/DELETE /agent/history, GET /agent/memory.
const express = require('express');
const { log } = require('../lib/logger');
const { logBedrockUsage } = require('../services/aws');
const { requireAuth } = require('../middleware/auth');
const { saveMessage, loadHistory, clearHistory, loadUserMemory, updateMemoryFromConversation, buildPersonalizedSystemPrompt } = require('../services/agent-store');
const { executeTool } = require('../services/agent-tools');
const { createMessage, MODEL_ID } = require('../agent/bedrock-client');
const { NOVA_SYSTEM_PROMPT, NOVA_TOOLS } = require('../agent/system-prompt');

const router = express.Router();

// POST /agent/chat
router.post('/agent/chat', requireAuth, async (req, res) => {
  const { message, currentCode = '', currentFile = '', recentOutput = '' } = req.body;

  if (!message || typeof message !== 'string' || !message.trim())
    return res.status(400).json({ error: 'Message is required' });
  if (message.length > 2000)
    return res.status(400).json({ error: 'Message too long (max 2000 characters)' });
  if (!process.env.BEDROCK_REGION && !process.env.BEDROCK_MODEL_ID)
    log('warn', 'Bedrock env vars not set — using defaults');

  res.setHeader('Content-Type', 'text/event-stream');
  res.setHeader('Cache-Control', 'no-cache');
  res.setHeader('Connection', 'keep-alive');
  res.flushHeaders();

  const send = (type, data) =>
    res.write(`data: ${JSON.stringify({ type, data })}\n\n`);

  const { userId, email } = req.user;

  try {
    const [history, memory] = await Promise.all([
      loadHistory(userId, 20).catch(() => []),
      loadUserMemory(userId),
    ]);

    const systemPrompt = buildPersonalizedSystemPrompt(NOVA_SYSTEM_PROMPT, memory, email);

    let contextualMessage = '';
    if (currentFile) contextualMessage += `[Current file: ${currentFile}]\n`;
    if (currentCode?.trim())
      contextualMessage += `[Current code:\n\`\`\`nova\n${currentCode.slice(0, 2000)}\n\`\`\`]\n`;
    if (recentOutput?.trim())
      contextualMessage += `[Recent output: ${recentOutput.slice(0, 500)}]\n`;
    contextualMessage += message;

    await saveMessage(userId, 'user', contextualMessage).catch(() => {});

    const messages = [
      ...history.map(m => ({ role: m.role, content: m.content })),
      { role: 'user', content: contextualMessage },
    ];

    const MAX_TOOL_ROUNDS = 5;
    let fullAssistantResponse = '';

    for (let round = 0; round < MAX_TOOL_ROUNDS; round++) {
      const response = await createMessage({
        model: MODEL_ID,
        max_tokens: 4096,
        system: systemPrompt,
        tools: NOVA_TOOLS,
        messages,
      });

      if (response.usage)
        logBedrockUsage(userId, response.usage.input_tokens || 0, response.usage.output_tokens || 0);

      const toolUseBlocks = [];
      for (const block of response.content) {
        if (block.type === 'text' && block.text) {
          fullAssistantResponse += block.text;
          send('text', block.text);
        } else if (block.type === 'tool_use') {
          toolUseBlocks.push(block);
        }
      }

      if (toolUseBlocks.length === 0 || response.stop_reason !== 'tool_use') break;

      messages.push({ role: 'assistant', content: response.content });

      const toolResults = [];
      for (const toolBlock of toolUseBlocks) {
        send('tool_start', { name: toolBlock.name, id: toolBlock.id });
        const result = await executeTool(toolBlock.name, toolBlock.input, userId);
        send('tool_result', { id: toolBlock.id, result });
        if (toolBlock.name === 'write_user_file' && result.success)
          send('file_written', { fileName: toolBlock.input.fileName });
        toolResults.push({
          type: 'tool_result',
          tool_use_id: toolBlock.id,
          content: JSON.stringify(result).slice(0, 5000),
        });
      }

      messages.push({ role: 'user', content: toolResults });
    }

    if (fullAssistantResponse)
      await saveMessage(userId, 'assistant', fullAssistantResponse).catch(() => {});

    updateMemoryFromConversation(userId, message, fullAssistantResponse).catch(() => {});

    send('done', '');
    res.end();

  } catch (err) {
    log('error', 'Bedrock call failed', { error: err.message, code: err.name });

    const bedrockMsg =
      err.name === 'AccessDeniedException'   ? 'Bedrock access denied. Check IAM role permissions.'
    : err.name === 'ThrottlingException'     ? 'AI service rate limited. Please wait a moment and try again.'
    : err.name === 'ModelNotReadyException'  ? 'AI model is warming up. Please try again in a few seconds.'
    : err.name === 'ValidationException'     ? 'Invalid request to AI service. Please try rephrasing.'
    :                                          'AI assistant unavailable. Please try again.';

    if (!res.headersSent) {
      return res.status(500).json({ error: bedrockMsg });
    }
    try { send('error', bedrockMsg); res.end(); } catch (_) {}
  }
});

// GET /agent/history
router.get('/agent/history', requireAuth, async (req, res) => {
  try {
    const history = await loadHistory(req.user.userId, 50);
    res.json({ history });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// DELETE /agent/history
router.delete('/agent/history', requireAuth, async (req, res) => {
  try {
    await clearHistory(req.user.userId);
    res.json({ ok: true });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /agent/memory
router.get('/agent/memory', requireAuth, async (req, res) => {
  try {
    const memory = await loadUserMemory(req.user.userId);
    res.json({ memory });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

module.exports = router;
