// The Install tab's setup checklist: the steps that make a working pair, each ticked from what the connected board
// reports, with where on the page it's done. Pure (status.js renders it), so it is unit-tested under Node.

// { role, rebootPending, keySet, peer (peerDiffs()), linkUp (as the health line judges it), survey (this session's
// verdict, or null) } -> [{ done, text, where }], in the order an install does them. Each says only what this board
// can see.
export function setupSteps({ role, rebootPending, keySet, peer, linkUp, survey }) {
  const other = role === 'house' ? 'gate' : role === 'gate' ? 'house' : 'other';
  return [
    { done: (role === 'house' || role === 'gate') && !rebootPending, text: 'Role set, saved and the board rebooted',
      where: 'Config' },
    { done: !!keySet, text: 'Link key written on this board (save its encrypted backup)', where: 'Board' },
    // Two boards without a key "match" (no key id either side): not a pair yet.
    { done: !!keySet && !!peer?.there?.params && !peer.diffs.length && !peer.fwMinor,
      text: `The ${other} board connected here once: its key, firmware and radio settings match`, where: 'Status → Other board' },
    { done: !!linkUp, text: 'Link up: the other board verified', where: 'Status → Link' },
    { done: survey === 'good' || survey === 'fair', text: 'Site survey from this board, run to the end: fair or better',
      where: 'Link → Site survey' },
  ];
}
