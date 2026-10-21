'use strict';
// AWS clients (DynamoDB, S3, CloudWatch), table bootstrap and Bedrock usage metrics.
const { AWS_REGION, USERS_TABLE, FILES_TABLE, AGENT_HISTORY_TABLE, AGENT_MEMORY_TABLE } = require('../config');
const { log } = require('../lib/logger');
const { CreateTableCommand, DescribeTableCommand } = require('@aws-sdk/client-dynamodb');

const { DynamoDBClient } = require('@aws-sdk/client-dynamodb');
const { DynamoDBDocumentClient } = require('@aws-sdk/lib-dynamodb');
const { S3Client } = require('@aws-sdk/client-s3');
const { CloudWatchClient, PutMetricDataCommand } = require('@aws-sdk/client-cloudwatch');

const dynamoRaw = new DynamoDBClient({ region: AWS_REGION });
const dynamo    = DynamoDBDocumentClient.from(dynamoRaw);
const s3        = new S3Client({ region: AWS_REGION });

const cloudwatch = new CloudWatchClient({ region: process.env.BEDROCK_REGION || 'us-east-1' });

async function logBedrockUsage(userId, inputTokens, outputTokens) {
  try {
    await cloudwatch.send(new PutMetricDataCommand({
      Namespace: 'NovaComp/Bedrock',
      MetricData: [
        { MetricName: 'InputTokens',  Value: inputTokens,  Unit: 'Count', Dimensions: [{ Name: 'UserId', Value: userId }] },
        { MetricName: 'OutputTokens', Value: outputTokens, Unit: 'Count', Dimensions: [{ Name: 'UserId', Value: userId }] },
      ],
    }));
  } catch (_) { /* non-critical */ }
}

// ── DynamoDB Table Bootstrap ──────────────────────────────────────────────────

async function ensureTables() {
  const tables = [
    {
      TableName: USERS_TABLE,
      KeySchema: [{ AttributeName: "email", KeyType: "HASH" }],
      AttributeDefinitions: [{ AttributeName: "email", AttributeType: "S" }],
      BillingMode: "PAY_PER_REQUEST",
    },
    {
      TableName: FILES_TABLE,
      KeySchema: [
        { AttributeName: "userId",   KeyType: "HASH" },
        { AttributeName: "filePath", KeyType: "RANGE" },
      ],
      AttributeDefinitions: [
        { AttributeName: "userId",   AttributeType: "S" },
        { AttributeName: "filePath", AttributeType: "S" },
      ],
      BillingMode: "PAY_PER_REQUEST",
    },
    {
      TableName: AGENT_HISTORY_TABLE,
      KeySchema: [
        { AttributeName: "userId",    KeyType: "HASH" },
        { AttributeName: "timestamp", KeyType: "RANGE" },
      ],
      AttributeDefinitions: [
        { AttributeName: "userId",    AttributeType: "S" },
        { AttributeName: "timestamp", AttributeType: "N" },
      ],
      BillingMode: "PAY_PER_REQUEST",
    },
    {
      TableName: AGENT_MEMORY_TABLE,
      KeySchema: [{ AttributeName: "userId", KeyType: "HASH" }],
      AttributeDefinitions: [{ AttributeName: "userId", AttributeType: "S" }],
      BillingMode: "PAY_PER_REQUEST",
    },
  ];

  for (const params of tables) {
    try {
      await dynamoRaw.send(new DescribeTableCommand({ TableName: params.TableName }));
      log("info", "DynamoDB table already exists", { table: params.TableName });
    } catch (e) {
      if (e.name === "ResourceNotFoundException") {
        await dynamoRaw.send(new CreateTableCommand(params));
        log("info", "DynamoDB table created", { table: params.TableName });
      } else {
        log("error", "DynamoDB DescribeTable failed", { table: params.TableName, error: e.message });
      }
    }
  }
}

module.exports = { dynamoRaw, dynamo, s3, ensureTables, logBedrockUsage };
