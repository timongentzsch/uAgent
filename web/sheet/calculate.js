// The engine at work for a sheet's page, in a worker the page makes for
// each answer: what is read of its tabs, a file's tabs, or the tabs as an
// Excel file.

import { tableOf, xlsxOf } from "./book.js";
import init, * as engine from "./wasm.js";
import { csvTab, workbookTabs } from "./workbook.js";

export async function answer({ module, tabs, file, name, csv, xlsx }) {
  await init({ module_or_path: module });
  if (file)
    return {
      tabs: csv
        ? [csvTab(engine, name, new TextDecoder().decode(file))]
        : workbookTabs(engine, file, name),
    };
  return xlsx
    ? { xlsx: xlsxOf(engine, tabs) }
    : { table: tableOf(engine, tabs) };
}
