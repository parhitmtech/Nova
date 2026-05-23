const {
  BedrockRuntimeClient,
  InvokeModelCommand,
  InvokeModelWithResponseStreamCommand,
} = require('@aws-sdk/client-bedrock-runtime');

const client = new BedrockRuntimeClient({
  region: process.env.BEDROCK_REGION || 'us-east-1',
});

const MODEL_ID =
  process.env.BEDROCK_MODEL_ID || 'anthropic.claude-sonnet-4-20250514-v1:0';

/**
 * Non-streaming message — mirrors anthropic.messages.create().
 * Returns { content, stop_reason, usage } in the same shape as the Anthropic SDK.
 */
async function createMessage({ max_tokens, system, tools, messages }) {
  const body = {
    anthropic_version: 'bedrock-2023-05-31',
    max_tokens: max_tokens || 4096,
    system,
    messages,
    ...(tools && tools.length > 0 ? { tools } : {}),
  };

  const response = await client.send(new InvokeModelCommand({
    modelId: MODEL_ID,
    contentType: 'application/json',
    accept: 'application/json',
    body: JSON.stringify(body),
  }));

  const parsed = JSON.parse(new TextDecoder().decode(response.body));

  return {
    content:     parsed.content,
    stop_reason: parsed.stop_reason,
    usage:       parsed.usage,
  };
}

/**
 * Streaming message — no tool use, plain text only.
 * Used for simple Q&A that doesn't need the tool-use loop.
 */
async function streamMessage({ max_tokens, system, messages }, onText, onDone, onError) {
  const body = {
    anthropic_version: 'bedrock-2023-05-31',
    max_tokens: max_tokens || 1024,
    system,
    messages,
  };

  try {
    const response = await client.send(new InvokeModelWithResponseStreamCommand({
      modelId: MODEL_ID,
      contentType: 'application/json',
      accept: 'application/json',
      body: JSON.stringify(body),
    }));

    for await (const chunk of response.body) {
      if (chunk.chunk?.bytes) {
        const decoded = JSON.parse(new TextDecoder().decode(chunk.chunk.bytes));
        if (decoded.type === 'content_block_delta' && decoded.delta?.type === 'text_delta')
          onText(decoded.delta.text);
        if (decoded.type === 'message_stop')
          onDone();
      }
    }
  } catch (err) {
    onError(err.message);
  }
}

module.exports = { createMessage, streamMessage, MODEL_ID };
