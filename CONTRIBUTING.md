We welcome your code contributions. Before submitting code via a GitHub pull
request, or by filing a bug in https://bugs.mysql.com you will need to have
signed the Oracle Contributor Agreement, see https://oca.opensource.oracle.com

Only pull requests from committers that can be verified as having signed the OCA
can be accepted.

Submitting a contribution
-------------------------

1. Make sure you have a user account at https://bugs.mysql.com. You'll need to reference
    this user account when you submit your OCA (Oracle Contributor Agreement).
2. Sign the Oracle OCA. You can find instructions for doing that at the OCA Page,
    at https://oca.opensource.oracle.com
3. Validate your contribution by including tests that sufficiently cover the functionality.
4. Verify that the entire test suite passes with your code applied.
5. Submit your pull request via GitHub or uploading it using the contribution tab to a bug
    record in https://bugs.mysql.com (using the 'contribution' tab).

Commit message format
---------------------

For non-trivial changes, use the repository commit message format documented in
`Docs/development/commit_message.md`. In short, include `Issue:` and
`Solution:` sections, wrap body text at 72 columns, and use section underlines
whose length matches the header text including the colon.

You can enable the local template with:

```
git config commit.template .gitmessage
```
