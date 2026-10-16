#!/usr/bin/env python3
"""Apply public Account configuration to rawfile and module metadata together."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
FIELDS = {'apiBaseUrl', 'accountClientId', 'membershipProductId',
          'membershipOneTimeProductId', 'membershipSubscriptionGroupId', 'membershipOperatorName',
          'membershipProductType', 'membershipPromotionalOfferId', 'coffeeProductId', 'cloudFunctionName', 'cloudFunctionVersion'}


def load_json5(path):
    text = path.read_text(encoding='utf-8')
    # Preserve quoted URLs while stripping comments from repository JSON5.
    text = re.sub(r'("(?:\\.|[^"\\])*")|/\*[\s\S]*?\*/|//[^\r\n]*',
                  lambda match: match.group(1) or '', text)
    return json.loads(re.sub(r',\s*([}\]])', r'\1', text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('public_config', type=Path,
                        help='JSON containing public Client ID and HTTPS API URL; no credentials')
    parser.add_argument('--apply', action='store_true', help='write the validated configuration')
    args = parser.parse_args()
    incoming = json.loads(args.public_config.read_text(encoding='utf-8'))
    if not isinstance(incoming, dict) or set(incoming) - FIELDS:
        parser.error('Only documented public configuration fields are accepted')
    rawfile = ROOT / 'entry/src/main/resources/rawfile/value_added_config.json'
    module_file = ROOT / 'entry/src/main/module.json5'
    config = {**json.loads(rawfile.read_text(encoding='utf-8')), **incoming}
    function = config.get('cloudFunctionName')
    if function is not None and (not isinstance(function, str) or not re.fullmatch(r'[a-zA-Z0-9_-]{1,128}', function)):
        parser.error('cloudFunctionName must be copied from the deployed Huawei function')
    version = config.get('cloudFunctionVersion', '$latest')
    if not isinstance(version, str) or not re.fullmatch(r'\$latest|[1-9][0-9]{0,8}', version):
        parser.error('cloudFunctionVersion must be $latest or a numeric function version')
    url = config['apiBaseUrl']
    valid_url = isinstance(url, str) and re.fullmatch(r'https://[a-z0-9][a-z0-9.-]+(?::443)?(?:/[a-zA-Z0-9_-]+)*', url)
    if (not valid_url and not function) or (url != '' and not valid_url):
        parser.error('Provide a deployed HTTPS base URL or a cloudFunctionName')
    if not isinstance(config['accountClientId'], str) or not re.fullmatch(
            r'[a-zA-Z0-9._-]{3,128}', config['accountClientId']):
        parser.error('accountClientId must be the OAuth Client ID copied from this AGC application')
    if config['membershipProductType'] not in (2, 3):
        parser.error('Membership must use automatic subscription type 2 or non-renewing type 3')
    offer_id = config.get('membershipPromotionalOfferId')
    if offer_id is not None and (config['membershipProductType'] != 2 or not isinstance(offer_id, str) or
                                 not re.fullmatch(r'[a-zA-Z0-9._-]{1,64}', offer_id)):
        parser.error('Promotional offer ID must match the saved automatic-subscription offer')
    for key in ('membershipProductId', 'coffeeProductId', 'membershipOneTimeProductId', 'membershipSubscriptionGroupId'):
        if key not in config:
            continue
        if not isinstance(config[key], str) or config[key] and not re.fullmatch(r'[a-zA-Z0-9._-]{1,128}', config[key]):
            parser.error('Product IDs must be public SKU identifiers or empty')
    if config['membershipProductId'] and config['membershipProductId'] == config['coffeeProductId']:
        parser.error('Membership and support products must have different IDs')
    if config.get('membershipOneTimeProductId') in (config['membershipProductId'], config['coffeeProductId']):
        parser.error('The one-time membership must have a distinct product ID')
    if 'membershipOperatorName' in config and (not isinstance(config['membershipOperatorName'], str) or
                                              len(config['membershipOperatorName']) > 128 or
                                              re.search(r'[\x00-\x1f]', config['membershipOperatorName'])):
        parser.error('membershipOperatorName must match the registered publisher')
    module = load_json5(module_file)
    metadata = module['module'].get('metadata', [])
    module['module']['metadata'] = [item for item in metadata if item.get('name') != 'client_id'] + [
        {'name': 'client_id', 'value': config['accountClientId']}]
    print('Validated public configuration; client_id matches rawfile. Device declarations are preserved.')
    if args.apply:
        # Read and validate both documents before changing either file.
        rawfile.write_text(json.dumps(config, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        module_file.write_text(json.dumps(module, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
        print('Configuration applied. Rebuild and sign the HAP to use it.')
    else:
        print('Dry run only. Add --apply to update the two source files.')


if __name__ == '__main__':
    main()
