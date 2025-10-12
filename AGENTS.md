# Agent Workflow Guidelines

1. **Reference Samples**  
   - Use the existing sources under `ax620e_bsp_sdk/msp/sample` as the primary reference when creating new examples in `cpp/`.
   - 着手前に `knowledge/` ディレクトリのメモ（例: `ax620e_bsp_sdk_sc850sl.md`）を確認し、必要なヘッダ／ライブラリ／リソースの位置を把握してから `ax620e_bsp_sdk/` を参照すること。

2. **Coding Standards**  
   - All C++ code must follow the Google C++ Style Guide.
   - Format sources with `clang-format --style=google` and ensure `cpplint` reports no issues.

3. **Build Requirements**  
   - When updating or adding C++ sources, configure and build with `-DCMAKE_BUILD_TYPE=Contribution` so that formatting and linting hooks run automatically.

4. **Commit Policy**  
   - Do **not** create commits without explicit reviewer approval. Keep changes staged locally until a review is complete.
   - Commit messages must fit on a single subject line within 72 characters. Use imperative mood and avoid trailing periods.
