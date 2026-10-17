#ifndef NOVA_INTEGRATIONS_H
#define NOVA_INTEGRATIONS_H
// External services used by `import sol` and `import hf` programs.

#include <string>

// src/integrations/sol.cc — SOL HPC cluster over the reverse SSH tunnel.
// SSH/SFTP need libssh2 (and a non-Windows build); without it they report an error.
std::string sol_trim(const std::string& s);
void        sol_write_file(const std::string& path, const std::string& content);
std::string sol_extract_slurm_id(const std::string& sbatchOut);
std::string sshExecSOL(const std::string& command);
void        sftpUploadSOL(const std::string& localPath, const std::string& remotePath);
void        sftpDownloadSOL(const std::string& remotePath, const std::string& localPath);
std::string generateTrainScript(const std::string& model, const std::string& task,
                                const std::string& dsName, const std::string& dsSplit,
                                int epochs, float lr, int batchSize,
                                const std::string& outputDir, const std::string& hfTok);
std::string generateSlurmScript(const std::string& jobId, const std::string& jobDir);

// src/integrations/hf.cc — HuggingFace Inference API.
std::string hf_escape_json(const std::string& s);
std::string hf_extract_text(const std::string& json);

#endif
