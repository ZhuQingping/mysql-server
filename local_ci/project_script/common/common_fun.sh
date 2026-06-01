#!/bin/ehco Waining:this is a library should be sourced !

function rm_docker()
{
  report=$1
  for docker_id in `grep "docker id:" ${report} | awk -F '[:]' '{print $4}'`
  do 
    docker stop $docker_id
    docker rm $docker_id
  done
}

function output_report()
{
  log_file=$1
  log_dir=$2
  pass_test=$(grep "\[ pass \]" ${log_file} | wc -l)
  Skipped_test=$(grep "\[ skipped \]" ${log_file} | wc -l)
  Failed_test=$(grep "\[ retry-fail \]" ${log_file} | wc -l)
  disabled_test=$(grep "\[ disabled \]" ${log_file} | wc -l)
  Total_test=`expr ${pass_test} + ${Skipped_test} + ${Failed_test} + ${disabled_test}`
  echo "========================MTR test ========================
Total_test:${Total_test}
Passed_test:${pass_test}
Skipped_test:${Skipped_test}
Disabled_test:${disabled_test}
Failed_test:${Failed_test}
$(grep "\[ retry-fail \]" ${log_file})
  " >> ${log_dir}/mtr_report
  cat ${log_dir}/mtr_report

  echo "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>
<testsuite tests=\"${Total_test}\" errors=\"0\" failures=\"${Failed_test}\" skip=\"${Skipped_test}\">
</testsuite>" > ${log_dir}/result.xml
  echo "Total_test:${Total_test} Failed_test:${Failed_test}" > ${log_dir}/simple_report
}

function update_code() {
  code_path=$1
  branch_name=$2
  echo "reposity : "$1", branch name is "$branch_name
  cd $1
  old_commit=`git log --pretty=format:"%H" -n 1`
  git fetch origin
  new_commit=`git log origin/${branch_name} --pretty=format:"%H" -n 1`
  if [ "$old_commit" != "$new_commit" ]; then
    git clean -dfx && git reset --hard HEAD && git checkout .
  fi
  git checkout -B ${branch_name} origin/${branch_name}
}

function update_code_with_tag() {
    local code_path="$1"
    local tag_name="$2"
    echo "repository: $code_path, tag: $tag_name"
    
    cd "$code_path" || { echo "Cannot cd to $code_path"; return 1; }
    
    old_commit=$(git log --pretty=format:"%H" -n 1)
    
    git fetch origin --tags
    
    if ! git rev-parse "refs/tags/${tag_name}" >/dev/null 2>&1; then
        echo "Tag not found: ${tag_name}"
        echo "Available tags:"
        git tag -l | head -10
        return 1
    fi
    
    # Get the commit corresponding to the tag
    new_commit=$(git rev-parse "refs/tags/${tag_name}")
    echo "Using tag: ${tag_name} (commit: ${new_commit:0:8})"
    
    # Clean up possible conflicting branches
    if git show-ref --verify --quiet "refs/heads/${tag_name}"; then
        echo "Removing conflicting branch: ${tag_name}"
        git branch -D "${tag_name}" 2>/dev/null || true
    fi
    
    if [ "$old_commit" != "$new_commit" ]; then
        echo "Detected code update, resetting local code..."
        git clean -dfx && git reset --hard HEAD && git checkout .
    fi
    
    # checkout to tag(detached HEAD)
    echo "Checking out tag in detached HEAD state..."
    git checkout "${tag_name}" || {
        echo "Failed to checkout tag: ${tag_name}"
        return 1
    }
    
    echo "Code update completed"
    return 0
}

function merge_code()
{
  echo "-- merge code"
  git checkout . && git clean -xdf
  remote_v=origin
  git fetch ${remote_v}
  git fetch ${codehubSourceRepoHttpUrl} refs/heads/${codehubSourceBranch}
  git checkout -f FETCH_HEAD
  git merge --no-ff --no-edit ${remote_v}/${codehubTargetBranch}
}

function log()
{
    echo "$(date '+%Y-%m-%d %H:%M:%S') [$1] $2"
}
