#include "minigit.hpp"
#include <iomanip>
#include <algorithm>

using namespace std;

// FNV-1a 64-bit hash
string MiniGit::computeHash(const string& content) {
    const uint64_t fnv_prime = 1099511628211ULL;
    uint64_t hash = 14695981039346656037ULL;
    
    for (char c : content) {
        hash ^= static_cast<uint64_t>(c);
        hash *= fnv_prime;
    }
    
    stringstream ss;
    ss << hex << setw(16) << setfill('0') << hash;
    return ss.str();
}

MiniGit::MiniGit() : gitDir(".minigit") {
    objectsDir = gitDir + "/objects";
    commitsDir = gitDir + "/commits";
    refsDir = gitDir + "/refs";
    headsDir = refsDir + "/heads";
}

MiniGit::~MiniGit() {}

void MiniGit::init() {
    if (fs::exists(gitDir)) {
        cerr << "MiniGit already initialized" << endl;
        return;
    }

    fs::create_directory(gitDir);
    fs::create_directory(objectsDir);
    fs::create_directory(commitsDir);
    fs::create_directory(refsDir);
    fs::create_directory(headsDir);

    ofstream(gitDir + "/index").close();

    Commit* initialCommit = new Commit();
    initialCommit->message = "Initial commit";
    time_t now = time(nullptr);
    initialCommit->timestamp = ctime(&now);
    initialCommit->timestamp.pop_back();
    
    stringstream commitData;
    commitData << initialCommit->message << initialCommit->timestamp;
    initialCommit->hash = computeHash(commitData.str());
    
    writeCommit(initialCommit);
    
    currentBranch = "main";
    branches[currentBranch] = initialCommit->hash;
    updateBranch(currentBranch);
    
    ofstream headFile(gitDir + "/HEAD");
    headFile << "ref: refs/heads/main" << endl;
    headFile.close();
    
    cout << "Initialized MiniGit repository with 'main' branch" << endl;
    delete initialCommit;
}

void MiniGit::add(const string& filename) {
    if (!fs::exists(filename)) {
        cerr << "File not found: " << filename << endl;
        return;
    }
    
    ifstream file(filename);
    string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
    file.close();
    
    string blobHash = computeHash(content);
    stagingArea[filename] = blobHash;
    persistStagingArea();
    
    writeBlob(blobHash, content);
    
    cout << "Added " << filename << " to staging area" << endl;
}

void MiniGit::commit(const string& message) {
    loadStagingArea();
    
    if (stagingArea.empty()) {
        cerr << "No changes staged for commit" << endl;
        return;
    }
    
    Commit* commit = new Commit();
    commit->message = message;
    time_t now = time(nullptr);
    commit->timestamp = ctime(&now);
    commit->timestamp.pop_back();
    
    string currentCommitHash = getCurrentCommitHash();
    if (!currentCommitHash.empty()) {
        commit->parentHashes.push_back(currentCommitHash);
        Commit* parent = readCommit(currentCommitHash);
        commit->fileMap = parent->fileMap;
        delete parent;
    }
    
    for (const auto& entry : stagingArea) {
        commit->fileMap[entry.first] = entry.second;
    }
    
    stringstream commitData;
    commitData << message << commit->timestamp;
    for (const auto& parent : commit->parentHashes) commitData << parent;
    for (const auto& entry : commit->fileMap) commitData << entry.first << entry.second;
    commit->hash = computeHash(commitData.str());
    
    writeCommit(commit);
    branches[currentBranch] = commit->hash;
    updateBranch(currentBranch);
    
    stagingArea.clear();
    persistStagingArea();
    
    cout << "[" << currentBranch << " " << commit->hash.substr(0, 7) << "] " << message << endl;
    delete commit;
}

void MiniGit::log() {
    string current = getCurrentCommitHash();
    if (current.empty()) {
        cout << "No commits yet" << endl;
        return;
    }
    
    while (!current.empty()) {
        Commit* commit = readCommit(current);
        if (!commit) break;
        
        cout << "commit " << commit->hash << endl;
        cout << "Author: MiniGit User <user@example.com>" << endl;
        cout << "Date:   " << commit->timestamp << endl;
        cout << "\n    " << commit->message << "\n" << endl;
        
        if (!commit->parentHashes.empty()) {
            current = commit->parentHashes[0];
        } else {
            current = "";
        }
        delete commit;
    }
}

void MiniGit::status() {
    cout << "On branch " << (currentBranch.empty() ? "DETACHED HEAD" : currentBranch) << endl;
    
    string currentCommitHash = getCurrentCommitHash();
    if (currentCommitHash.empty()) {
        cout << "No commits yet" << endl;
        return;
    }
    
    Commit* currentCommit = readCommit(currentCommitHash);
    if (!currentCommit) return;
    
    cout << "\nStaged changes:" << endl;
    if (stagingArea.empty()) {
        cout << "  (no files staged)" << endl;
    } else {
        for (const auto& entry : stagingArea) {
            cout << "  " << getFileStatus(entry.first) << " " << entry.first << endl;
        }
    }
    
    cout << "\nBranches:" << endl;
    for (const auto& branch : branches) {
        cout << (branch.first == currentBranch ? "* " : "  ") << branch.first << endl;
    }
    
    delete currentCommit;
}

string MiniGit::getFileStatus(const string& filename) {
    string currentCommitHash = getCurrentCommitHash();
    if (currentCommitHash.empty()) return "A";
    
    Commit* currentCommit = readCommit(currentCommitHash);
    if (!currentCommit) return "?";
    
    string status = "?";
    if (currentCommit->fileMap.count(filename)) {
        if (stagingArea.count(filename)) {
            status = (currentCommit->fileMap.at(filename) != stagingArea.at(filename)) ? "M" : " ";
        } else {
            status = "D"; // Deleted from staging but still in commit
        }
    } else if (stagingArea.count(filename)) {
        status = "A"; // Added
    }
    
    delete currentCommit;
    return status;
}

void MiniGit::branch(const string& branchName) {
    if (branches.find(branchName) != branches.end()) {
        cerr << "Branch already exists" << endl;
        return;
    }
    
    branches[branchName] = getCurrentCommitHash();
    updateBranch(branchName);
    cout << "Created branch: " << branchName << endl;
}

void MiniGit::checkout(const string& target) {
    if (branches.find(target) != branches.end()) {
        currentBranch = target;
        updateHead();
        restoreCommit(branches[target]);
        cout << "Switched to branch '" << target << "'" << endl;
        return;
    }
    
    if (fs::exists(commitsDir + "/" + target)) {
        currentBranch = "";
        ofstream headFile(gitDir + "/HEAD");
        headFile << target << endl;
        headFile.close();
        restoreCommit(target);
        cout << "Detached HEAD at " << target << endl;
        return;
    }
    
    cerr << "Invalid branch or commit: " << target << endl;
}

void MiniGit::merge(const string& branchName) {
    if (branches.find(branchName) == branches.end()) {
        cerr << "Branch not found: " << branchName << endl;
        return;
    }
    
    string currentHash = getCurrentCommitHash();
    string targetHash = branches[branchName];
    string baseHash = findLCA(currentHash, targetHash);
    
    if (baseHash == targetHash) {
        cout << "Already up-to-date" << endl;
        return;
    }
    
    if (baseHash == currentHash) {
        checkout(branchName);
        cout << "Fast-forward merge" << endl;
        return;
    }
    
    threeWayMerge(currentHash, targetHash, baseHash);
    inMergeState = true;
    mergeTargetBranch = branchName;
    cout << "Merge started. Resolve conflicts and commit" << endl;
}

// Helper methods implementation
void MiniGit::writeBlob(const string& hash, const string& content) {
    ofstream blobFile(objectsDir + "/" + hash);
    blobFile << content;
    blobFile.close();
}

string MiniGit::readBlob(const string& hash) {
    ifstream blobFile(objectsDir + "/" + hash);
    if (!blobFile) return "";
    string content((istreambuf_iterator<char>(blobFile)), istreambuf_iterator<char>());
    blobFile.close();
    return content;
}

void MiniGit::writeCommit(const Commit* commit) {
    ofstream commitFile(commitsDir + "/" + commit->hash);
    commitFile << commit->message << endl;
    commitFile << commit->timestamp << endl;
    for (const auto& parent : commit->parentHashes) {
        commitFile << parent << endl;
    }
    commitFile << "---" << endl;
    for (const auto& entry : commit->fileMap) {
        commitFile << entry.first << ":" << entry.second << endl;
    }
    commitFile.close();
}

MiniGit::Commit* MiniGit::readCommit(const string& hash) {
    ifstream commitFile(commitsDir + "/" + hash);
    if (!commitFile) return nullptr;
    
    Commit* commit = new Commit();
    commit->hash = hash;
    getline(commitFile, commit->message);
    getline(commitFile, commit->timestamp);
    
    string line;
    while (getline(commitFile, line) && line != "---") {
        commit->parentHashes.push_back(line);
    }
    
    while (getline(commitFile, line)) {
        size_t pos = line.find(':');
        if (pos != string::npos) {
            string filename = line.substr(0, pos);
            string blobHash = line.substr(pos + 1);
            commit->fileMap[filename] = blobHash;
        }
    }
    commitFile.close();
    return commit;
}

string MiniGit::getCurrentCommitHash() {
    if (!currentBranch.empty() && branches.find(currentBranch) != branches.end()) {
        return branches[currentBranch];
    }
    
    if (fs::exists(gitDir + "/HEAD")) {
        ifstream headFile(gitDir + "/HEAD");
        string headRef;
        getline(headFile, headRef);
        headFile.close();
        
        if (headRef.substr(0, 5) != "ref: ") {
            return headRef;
        }
    }
    
    return "";
}

void MiniGit::updateHead() {
    ofstream headFile(gitDir + "/HEAD");
    headFile << "ref: refs/heads/" << currentBranch << endl;
    headFile.close();
}

void MiniGit::updateBranch(const string& branchName) {
    ofstream branchFile(headsDir + "/" + branchName);
    branchFile << branches[branchName];
    branchFile.close();
}

void MiniGit::restoreCommit(const string& commitHash) {
    Commit* commit = readCommit(commitHash);
    if (!commit) return;

    // Files to NEVER delete
    const set<string> protectedFiles = {
        "minigit",          // executable
        "minigit.cpp",      // source files
        "minigit.hpp",
        "main.cpp",
        "Makefile",
        ".minigit"          // repository data
    };

    // Restore tracked files
    for (const auto& entry : commit->fileMap) {
        string content = readBlob(entry.second);
        ofstream file(entry.first);
        file << content;
        file.close();
    }

    // Only delete untracked TEST files (not protected files)
    for (const auto& entry : fs::directory_iterator(".")) {
        if (entry.is_regular_file()) {
            string filename = entry.path().filename().string();
            bool isTracked = commit->fileMap.count(filename);
            bool isProtected = protectedFiles.count(filename) > 0 || 
                             (filename.find(".minigit") == 0);

            if (!isTracked && !isProtected) {
                fs::remove(entry.path());
            }
        }
    }

    delete commit;
}

string MiniGit::findLCA(const string& commit1, const string& commit2) {
    set<string> ancestors;
    vector<string> stack;
    stack.push_back(commit1);
    
    while (!stack.empty()) {
        string current = stack.back();
        stack.pop_back();
        ancestors.insert(current);
        
        Commit* c = readCommit(current);
        if (c) {
            for (const auto& parent : c->parentHashes) {
                stack.push_back(parent);
            }
            delete c;
        }
    }
    
    stack.push_back(commit2);
    while (!stack.empty()) {
        string current = stack.back();
        stack.pop_back();
        
        if (ancestors.find(current) != ancestors.end()) {
            return current;
        }
        
        Commit* c = readCommit(current);
        if (c) {
            for (const auto& parent : c->parentHashes) {
                stack.push_back(parent);
            }
            delete c;
        }
    }
    
    return "";
}

void MiniGit::threeWayMerge(
    const string& currentHash,
    const string& targetHash,
    const string& baseHash
) {
    Commit* baseCommit = readCommit(baseHash);
    Commit* currentCommit = readCommit(currentHash);
    Commit* targetCommit = readCommit(targetHash);
    if (!baseCommit || !currentCommit || !targetCommit) return;
    
    set<string> allFiles;
    for (const auto& entry : baseCommit->fileMap) allFiles.insert(entry.first);
    for (const auto& entry : currentCommit->fileMap) allFiles.insert(entry.first);
    for (const auto& entry : targetCommit->fileMap) allFiles.insert(entry.first);
    
    for (const string& filename : allFiles) {
        string baseContent = baseCommit->fileMap.count(filename) ? 
            readBlob(baseCommit->fileMap[filename]) : "";
        string currentContent = currentCommit->fileMap.count(filename) ? 
            readBlob(currentCommit->fileMap[filename]) : "";
        string targetContent = targetCommit->fileMap.count(filename) ? 
            readBlob(targetCommit->fileMap[filename]) : "";
        
        if (currentContent == targetContent) {
            continue;
        } else if (baseContent == currentContent) {
            stagingArea[filename] = targetCommit->fileMap[filename];
        } else if (baseContent == targetContent) {
            stagingArea[filename] = currentCommit->fileMap[filename];
        } else {
            markConflict(filename, currentContent, targetContent);
        }
    }
    
    persistStagingArea();
    
    delete baseCommit;
    delete currentCommit;
    delete targetCommit;
}

void MiniGit::markConflict(
    const string& filename,
    const string& currentContent,
    const string& incomingContent
) {
    ofstream file(filename);
    file << "<<<<<<< HEAD\n";
    file << currentContent;
    file << "\n=======\n";
    file << incomingContent;
    file << "\n>>>>>>> incoming\n";
    file.close();
    cout << "CONFLICT: " << filename << " - manual resolution required" << endl;
}

void MiniGit::persistStagingArea() {
    ofstream stageFile(gitDir + "/index");
    for (const auto& entry : stagingArea) {
        stageFile << entry.first << ":" << entry.second << "\n";
    }
    stageFile.close();
}

void MiniGit::loadStagingArea() {
    stagingArea.clear();
    ifstream stageFile(gitDir + "/index");
    if (!stageFile) return;
    
    string line;
    while (getline(stageFile, line)) {
        size_t pos = line.find(':');
        if (pos != string::npos) {
            string filename = line.substr(0, pos);
            string hash = line.substr(pos + 1);
            stagingArea[filename] = hash;
        }
    }
    stageFile.close();
}

void MiniGit::loadState() {
    if (fs::exists(gitDir + "/HEAD")) {
        ifstream headFile(gitDir + "/HEAD");
        string headRef;
        getline(headFile, headRef);
        if (headRef.find("ref: refs/heads/") == 0) {
            currentBranch = headRef.substr(16);
        }
        headFile.close();
    }
    
    if (fs::exists(headsDir)) {
        for (const auto& entry : fs::directory_iterator(headsDir)) {
            if (entry.is_regular_file()) {
                ifstream branchFile(entry.path());
                string commitHash;
                getline(branchFile, commitHash);
                branches[entry.path().filename()] = commitHash;
            }
        }
    }
    
    loadStagingArea();
}