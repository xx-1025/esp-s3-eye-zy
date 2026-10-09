# Git / GitHub 推送说明

## 当前远端

```
origin  git@github-xx1025:xx-1025/esp-s3-eye-zy.git      # 分支 main
网页    https://github.com/xx-1025/esp-s3-eye-zy
```

> 之前关联过的 `Aholic-12/esp-s3-eye-zb` 已弃用，不要再往那边推。

## 为什么要绕一下

这台机器上 **`github.com:443` 不通**（TCP 超时，DNS 正常，属于网络层封锁），
所以不能用常规的 `https://github.com/...` 推送。
已验证可用的通道是 **GitHub 官方的 443 SSH 端口 `ssh.github.com:443`**。

## 本机已做的配置

`~/.ssh/config` 里给每个账号配了一个 Host 别名，都指向 `ssh.github.com:443`：

```
# 账号 1：Aholic-12
Host github.com
    HostName ssh.github.com
    Port 443
    User git
    IdentityFile ~/.ssh/id_ed25519
    IdentitiesOnly yes

# 账号 2：xx-1025
Host github-xx1025
    HostName ssh.github.com
    Port 443
    User git
    IdentityFile ~/.ssh/id_ed25519_xx1025
    IdentitiesOnly yes
```

**为什么要两把密钥**：GitHub 不允许同一把公钥挂在两个账号上。
同一个密钥只能属于一个账号，所以每个账号单独生成一把，
远端地址写成 `git@<别名>:<owner>/<repo>.git` 来选密钥。

验证通道：

```bash
ssh -T git@github-xx1025     # Hi xx-1025! ...       ← 正常
ssh -T git@github-xx1025     # Permission denied (publickey).  ← 公钥还没加到该账号
```

## 另一个坑：全局 URL 重写

`~/.gitconfig` 里有一条
`url."https://jihulab.com/esp-mirror/".insteadOf = https://github.com/`
（拉乐鑫组件用的镜像）。它会把**任何** `https://github.com/` 地址劫持到 jihulab。
只要远端用 SSH 形式就不受影响；万一要临时用 https，在仓库里加一条更长前缀的
identity 规则覆盖即可：

```bash
git config url."https://github.com/xx-1025/".insteadOf "https://github.com/xx-1025/"
```

## 日常操作

```bash
cd esp32-sensor-web

git status --short          # 先看有没有误提交敏感文件（重要）
git add -A
git commit -m "说明这次改了什么"
git push
```

## 推送前的红线

`firmware/include/config.h` 里有 WiFi 密码和 VPS 地址，**已被 `.gitignore` 排除**。
提交前务必确认：

```bash
git ls-files --cached | grep "config.h"     # 只应出现 config.example.h
```

被忽略、不进仓库的还有：`firmware/.pio/`、`server/data/`、`server/node_modules/`、
`tools/__pycache__/`、`*.pyc`、`*.log`。

## 换机器 / 重新克隆

```bash
git clone git@github-xx1025:xx-1025/esp-s3-eye-zy.git
# 新机器上要么把 ~/.ssh/config 里那段别名规则也加上，
# 要么直接用 ssh.github.com:443 的等价写法
```

## 已知小问题（本机特有，不影响推送）

`git status` 有时会显示 `## main...origin/main [gone]`。
原因是这台机器上 **git 自己写 `refs/remotes/origin/main` 落不了盘**
（`git fetch` 明明报告 `[new branch] main -> origin/main`，但那个文件随即消失），
手动写文件反而能持久。

**这只是显示问题，不影响推送**。要核对远端到底有没有推上去，用这个：

```bash
git ls-remote origin          # 直接问远端，最可靠
```

想让 `git status` 显示正常，可以手动补一下（远端 SHA 换成 `ls-remote` 查到的）：

```bash
mkdir -p .git/refs/remotes/origin
git ls-remote origin | awk '/refs\/heads\/main/{print $1}' > .git/refs/remotes/origin/main
```

